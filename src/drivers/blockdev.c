#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "timer.h"
#include "export.h"
#include "process.h"
#include "ioprio.h"

/* Global device table */
static struct blockdev_entry g_blockdevs[BLOCKDEV_MAX_DEVICES];
static struct blk_request_queue g_queues[BLOCKDEV_MAX_DEVICES];
static spinlock_t g_dev_lock;
static int g_initialized = 0;

/* ── Request slab cache ───────────────────────────────────────────── */

struct blk_request *blk_request_alloc(void) {
    struct blk_request *req = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    if (!req) return NULL;
    memset(req, 0, sizeof(*req));
    /* Capture the current process's I/O priority so the block layer can
     * order requests appropriately (RT > BE > IDLE). */
    struct process *cur = process_get_current();
    req->ioprio = cur ? cur->ioprio : IOPRIO_DEFAULT;
    return req;
}

void blk_request_free(struct blk_request *req) {
    if (req) kfree(req);
}

/* ── Request queue operations ─────────────────────────────────────── */

static void queue_insert(struct blk_request_queue *q, struct blk_request *req) {
    q->queued_count++;

    if (q->sched == BLK_SCHED_NOOP || !q->head) {
        if (q->tail) {
            q->tail->next = req;
            q->tail = req;
        } else {
            q->head = req;
            q->tail = req;
        }
        req->next = NULL;
        return;
    }

    if (q->sched == BLK_SCHED_DEADLINE) {
        req->expiry = timer_get_ticks() + 5;
        uint8_t req_order = ioprio_class_order(req->ioprio);
        struct blk_request **pp = &q->head;
        while (*pp) {
            int insert_here = 0;
            uint8_t pp_order = ioprio_class_order((*pp)->ioprio);
            /* Primary sort: I/O priority class (RT < BE < IDLE).
             * Secondary sort: reads before writes within same class.
             * Tertiary sort: by expiry, then by LBA. */
            if (req_order < pp_order) {
                insert_here = 1;
            } else if (req_order == pp_order) {
                if (req->flags & BLK_REQ_READ && !((*pp)->flags & BLK_REQ_READ)) {
                    insert_here = (req->expiry <= (*pp)->expiry + 2);
                } else {
                    insert_here = (req->expiry < (*pp)->expiry) ||
                                  (req->expiry == (*pp)->expiry && req->lba < (*pp)->lba);
                }
            }
            if (insert_here) break;
            pp = &(*pp)->next;
        }
        req->next = *pp;
        *pp = req;
        if (!req->next) q->tail = req;
        return;
    }
}

static struct blk_request *queue_peek(struct blk_request_queue *q) {
    if (!q->head) return NULL;
    return q->head;
}

static struct blk_request *queue_pop(struct blk_request_queue *q) {
    struct blk_request *req = q->head;
    if (!req) return NULL;
    q->head = req->next;
    if (!q->head) q->tail = NULL;
    req->next = NULL;
    q->queued_count--;
    return req;
}

/* ── Statistics API ───────────────────────────────────────────────── */

void blockdev_stats_update(int dev_id, int is_write, uint64_t sectors, uint64_t duration_ms) {
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dev_lock, &irq_flags);

    if (is_write) {
        g_blockdevs[dev_id].stats.write_ops++;
        g_blockdevs[dev_id].stats.write_sectors += sectors;
        g_blockdevs[dev_id].stats.write_ms += duration_ms;
    } else {
        g_blockdevs[dev_id].stats.read_ops++;
        g_blockdevs[dev_id].stats.read_sectors += sectors;
        g_blockdevs[dev_id].stats.read_ms += duration_ms;
    }

    spinlock_irqsave_release(&g_dev_lock, irq_flags);
}

int blockdev_get_stats(int dev, struct blockdev_stats *s) {
    if (!g_initialized) return -1;
    if (dev < 0 || dev >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev].active)
        return -1;
    if (!s) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dev_lock, &irq_flags);
    *s = g_blockdevs[dev].stats;
    spinlock_irqsave_release(&g_dev_lock, irq_flags);

    return 0;
}

/* ── Core submission path ─────────────────────────────────────────── */

int blk_submit_async(struct blk_request *req) {
    if (!req || !g_initialized) return -1;
    int dev_id = req->dev_id;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;

    struct blk_request_queue *q = &g_queues[dev_id];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&q->lock, &irq_flags);

    req->result = 0;
    req->inflight = 0;
    req->done = 0;

    uint64_t start_ticks = timer_get_ticks();

    if (q->inflight_count == 0 && q->queued_count == 0) {
        req->inflight = 1;
        q->inflight_count++;
        spinlock_irqsave_release(&q->lock, irq_flags);

        int ret = g_blockdevs[dev_id].submit_fn(req);

        /* Track statistics for synchronous completion */
        uint64_t elapsed_ticks = timer_get_ticks() - start_ticks;
        uint64_t elapsed_ms = elapsed_ticks * 1000 / TIMER_FREQ;

        if (g_blockdevs[dev_id].flags & BLK_DRIVER_ASYNC) {
            return ret < 0 ? ret : 0;
        }

        q->inflight_count--;
        req->done = 1;

        /* Update stats */
        blockdev_stats_update(dev_id, !(req->flags & BLK_REQ_READ),
                              req->count, elapsed_ms);

        if (req->done_wq) {
            wait_queue_wake(req->done_wq);
        }
        return ret < 0 ? ret : 0;
    }

    queue_insert(q, req);
    spinlock_irqsave_release(&q->lock, irq_flags);
    return 0;
}

int blk_submit_sync(int dev_id, uint64_t lba, uint32_t count,
                    void *buf, uint32_t flags) {
    if (!g_initialized) return -1;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;

    uint32_t max_xfer = g_blockdevs[dev_id].max_transfer;

    /* ── Splitting: if the request exceeds max_transfer, split into
     *     multiple sub-requests (Item 328: bio splitting).  This is
     *     transparent to callers — errors from any sub-request are
     *     returned, and partial progress is NOT rolled back. ── */
    if (max_xfer > 0 && count > max_xfer) {
        uint64_t remaining = count;
        uint64_t cur_lba = lba;
        uint8_t *cur_buf = (uint8_t *)buf;
        int first_error = 0;

        while (remaining > 0) {
            uint32_t chunk = (remaining > max_xfer) ? max_xfer : (uint32_t)remaining;

            struct blk_request *req = blk_request_alloc();
            if (!req) return -6;

            req->dev_id = (uint8_t)dev_id;
            req->lba = cur_lba;
            req->count = chunk;
            req->buf = cur_buf;
            req->flags = flags;

            struct wait_queue wq;
            wait_queue_init(&wq);
            req->done_wq = &wq;

            int ret = blk_submit_async(req);
            if (ret < 0) {
                blk_request_free(req);
                if (!first_error) first_error = ret;
                break;
            }

            while (!req->done) {
                wait_queue_sleep(&wq);
            }

            ret = req->result;
            blk_request_free(req);

            if (ret < 0) {
                if (!first_error) first_error = ret;
                break;
            }

            cur_lba += chunk;
            cur_buf += (uint64_t)chunk * 512;
            remaining -= chunk;
        }

        return first_error;
    }

    /* ── Fast path: single request fits within limits ── */
    struct blk_request *req = blk_request_alloc();
    if (!req) return -6;

    req->dev_id = (uint8_t)dev_id;
    req->lba = lba;
    req->count = count;
    req->buf = buf;
    req->flags = flags;

    struct wait_queue wq;
    wait_queue_init(&wq);
    req->done_wq = &wq;

    int ret = blk_submit_async(req);
    if (ret < 0) {
        blk_request_free(req);
        return ret;
    }

    while (!req->done) {
        wait_queue_sleep(&wq);
    }

    ret = req->result;
    blk_request_free(req);
    return ret;
}

/**
 * blockdev_discard — Deallocate (TRIM) a range of LBAs on a block device.
 *
 * Submits a discard request through the block device layer.  The underlying
 * driver (e.g., NVMe deallocate, ATA TRIM) will mark the specified sector
 * range as free, enabling the device to optimize future writes.
 *
 * @dev_id  Block device ID.
 * @lba     Starting LBA (sector number).
 * @count   Number of LBAs (sectors) to deallocate.
 *
 * Returns 0 on success, -errno on error.
 */
int blockdev_discard(int dev_id, uint64_t lba, uint32_t count) {
    if (!g_initialized) return -1;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;
    if (count == 0)
        return 0;

    uint32_t max_xfer = g_blockdevs[dev_id].max_transfer;

    /* ── Splitting: discard requests may exceed max_transfer (Item 328) ── */
    if (max_xfer > 0 && count > max_xfer) {
        uint64_t remaining = count;
        uint64_t cur_lba = lba;
        int first_error = 0;

        while (remaining > 0) {
            uint32_t chunk = (remaining > max_xfer) ? max_xfer : (uint32_t)remaining;

            struct blk_request *req = blk_request_alloc();
            if (!req) return -6;

            req->dev_id = (uint8_t)dev_id;
            req->lba = cur_lba;
            req->count = chunk;
            req->buf = NULL;
            req->flags = BLK_REQ_DISCARD;

            struct wait_queue wq;
            wait_queue_init(&wq);
            req->done_wq = &wq;

            int ret = blk_submit_async(req);
            if (ret < 0) {
                blk_request_free(req);
                if (!first_error) first_error = ret;
                break;
            }

            while (!req->done) {
                wait_queue_sleep(&wq);
            }

            ret = req->result;
            blk_request_free(req);

            if (ret < 0) {
                if (!first_error) first_error = ret;
                break;
            }

            cur_lba += chunk;
            remaining -= chunk;
        }

        return first_error;
    }

    /* Build a discard request and submit synchronously */
    struct blk_request *req = blk_request_alloc();
    if (!req) return -6;

    req->dev_id = (uint8_t)dev_id;
    req->lba = lba;
    req->count = count;
    req->buf = NULL;
    req->flags = BLK_REQ_DISCARD;

    struct wait_queue wq;
    wait_queue_init(&wq);
    req->done_wq = &wq;

    int ret = blk_submit_async(req);
    if (ret < 0) {
        blk_request_free(req);
        return ret;
    }

    while (!req->done) {
        wait_queue_sleep(&wq);
    }

    ret = req->result;
    blk_request_free(req);
    return ret;
}

void blk_request_done(struct blk_request *req) {
    if (!req) return;

    req->done = 1;
    req->inflight = 0;

    struct blk_request_queue *q = &g_queues[req->dev_id];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&q->lock, &irq_flags);
    q->inflight_count--;
    spinlock_irqsave_release(&q->lock, irq_flags);

    if (req->done_wq) {
        wait_queue_wake(req->done_wq);
    }
}

int blk_request_dequeue(struct blk_request_queue *q, struct blk_request **out) {
    if (!q || !out) return 0;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&q->lock, &irq_flags);

    struct blk_request *req = queue_peek(q);
    if (!req) {
        spinlock_irqsave_release(&q->lock, irq_flags);
        *out = NULL;
        return 0;
    }

    req = queue_pop(q);
    req->inflight = 1;
    q->inflight_count++;
    spinlock_irqsave_release(&q->lock, irq_flags);

    *out = req;
    return 1;
}

struct blk_request *blk_request_deadline_peek(struct blk_request_queue *q) {
    if (!q) return NULL;
    return queue_peek(q);
}

static void drain_queue(struct blk_request_queue *q) {
    struct blk_request *req = q->head;
    while (req) {
        struct blk_request *next = req->next;
        kfree(req);
        req = next;
    }
    q->head = NULL;
    q->tail = NULL;
    q->queued_count = 0;
}

/* ── Public API ───────────────────────────────────────────────────── */

void blockdev_init(void) {
    memset(g_blockdevs, 0, sizeof(g_blockdevs));
    memset(g_queues, 0, sizeof(g_queues));
    spinlock_init(&g_dev_lock);
    g_initialized = 1;
}

int blockdev_register(int id, const char *name,
                      blk_driver_submit_fn submit_fn,
                      blk_driver_idle_fn idle_fn,
                      uint64_t sector_count,
                      int flags) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    if (!submit_fn) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dev_lock, &irq_flags);

    if (g_blockdevs[id].active) {
        drain_queue(&g_queues[id]);
    }

    g_blockdevs[id].active = 1;
    g_blockdevs[id].flags = flags;
    g_blockdevs[id].submit_fn = submit_fn;
    g_blockdevs[id].idle_fn = idle_fn;
    g_blockdevs[id].sector_count = sector_count;
    g_blockdevs[id].sector_size = 512;

    /* Zero out stats */
    memset(&g_blockdevs[id].stats, 0, sizeof(struct blockdev_stats));

    if (name && *name) {
        strncpy(g_blockdevs[id].name, name, sizeof(g_blockdevs[id].name) - 1);
        g_blockdevs[id].name[sizeof(g_blockdevs[id].name) - 1] = '\0';
    } else {
        g_blockdevs[id].name[0] = '\0';
    }

    struct blk_request_queue *q = &g_queues[id];
    spinlock_init(&q->lock);
    q->head = NULL;
    q->tail = NULL;
    q->sched = BLK_SCHED_DEADLINE;
    q->dev_id = (uint8_t)id;
    q->queued_count = 0;
    q->inflight_count = 0;

    spinlock_irqsave_release(&g_dev_lock, irq_flags);
    return 0;
}

#if !defined(BLOCKDEV_NEW_ONLY)
struct legacy_driver {
    blockdev_read_fn  read_fn;
    blockdev_write_fn write_fn;
};

static struct legacy_driver g_legacy[BLOCKDEV_MAX_DEVICES];

static int legacy_submit_fn_adapter(struct blk_request *req) {
    int dev_id = req->dev_id;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;

    uint64_t start_ticks = timer_get_ticks();

    if (req->flags & BLK_REQ_READ) {
        if (!g_legacy[dev_id].read_fn) { req->result = -1; return -1; }
        req->result = g_legacy[dev_id].read_fn((uint32_t)req->lba,
                                                (uint8_t)req->count,
                                                req->buf);
    } else if (req->flags & BLK_REQ_WRITE) {
        if (!g_legacy[dev_id].write_fn) { req->result = -1; return -1; }
        req->result = g_legacy[dev_id].write_fn((uint32_t)req->lba,
                                                 (uint8_t)req->count,
                                                 (const void *)req->buf);
    } else {
        req->result = -1;
        return -1;
    }

    uint64_t elapsed_ms = (timer_get_ticks() - start_ticks) * 1000 / TIMER_FREQ;
    blockdev_stats_update(dev_id, !(req->flags & BLK_REQ_READ),
                          req->count, elapsed_ms);

    return req->result;
}

int blockdev_register_legacy(int id, const char *name,
                              blockdev_read_fn read_fn,
                              blockdev_write_fn write_fn,
                              blockdev_size_fn size_fn) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    if (!read_fn) return -1;

    g_legacy[id].read_fn = read_fn;
    g_legacy[id].write_fn = write_fn;

    uint64_t nsectors = size_fn ? size_fn() : 0;

    int ret = blockdev_register(id, name, legacy_submit_fn_adapter, NULL, nsectors, 0);
    if (ret < 0) return ret;

    return 0;
}
#endif

int blockdev_is_registered(int id) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return 0;
    return g_blockdevs[id].active;
}

int blockdev_unregister(int id) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dev_lock, &irq_flags);
    g_blockdevs[id].active = 0;
    g_blockdevs[id].submit_fn = NULL;
    g_blockdevs[id].idle_fn = NULL;
    drain_queue(&g_queues[id]);
    spinlock_irqsave_release(&g_dev_lock, irq_flags);
    return 0;
}

const char *blockdev_name(int id) {
    if (!blockdev_is_registered(id)) return "";
    return g_blockdevs[id].name;
}

uint64_t blockdev_get_sectors(int id) {
    if (!blockdev_is_registered(id)) return 0;
    return g_blockdevs[id].sector_count;
}

struct blk_request_queue *blockdev_get_queue(int id) {
    if (!blockdev_is_registered(id)) return NULL;
    return &g_queues[id];
}

int blockdev_set_scheduler(int id, enum blk_scheduler sched) {
    if (!blockdev_is_registered(id)) return -1;
    if (sched >= BLK_SCHED_COUNT) return -1;
    g_queues[id].sched = sched;
    return 0;
}

enum blk_scheduler blockdev_get_scheduler(int id) {
    if (!blockdev_is_registered(id)) return BLK_SCHED_NOOP;
    return g_queues[id].sched;
}

/* ── Transfer limit configuration (Item 328) ──────────────────────── */

int blockdev_set_max_transfer(int dev_id, uint32_t max_sectors)
{
    if (!g_initialized) return -1;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;
    if (max_sectors == 0) {
        g_blockdevs[dev_id].max_transfer = 0; /* unlimited */
    } else {
        g_blockdevs[dev_id].max_transfer = max_sectors;
    }
    return 0;
}

uint32_t blockdev_get_max_transfer(int dev_id)
{
    if (!blockdev_is_registered(dev_id)) return 0;
    return g_blockdevs[dev_id].max_transfer;
}

/* ── Exported symbols for driver modules ─────────────────────────── */
EXPORT_SYMBOL(blockdev_register);
EXPORT_SYMBOL(blockdev_register_legacy);
EXPORT_SYMBOL(blockdev_unregister);
EXPORT_SYMBOL(blockdev_name);
EXPORT_SYMBOL(blockdev_is_registered);
EXPORT_SYMBOL(blockdev_get_scheduler);
EXPORT_SYMBOL(blk_request_alloc);
EXPORT_SYMBOL(blk_request_free);
EXPORT_SYMBOL(blk_request_done);
EXPORT_SYMBOL(blk_submit_sync);
EXPORT_SYMBOL(blockdev_discard);
EXPORT_SYMBOL(blockdev_set_max_transfer);
EXPORT_SYMBOL(blockdev_get_max_transfer);