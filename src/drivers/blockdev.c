#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "timer.h"

/* Global device table */
static struct blockdev_entry g_blockdevs[BLOCKDEV_MAX_DEVICES];
static struct blk_request_queue g_queues[BLOCKDEV_MAX_DEVICES];
static spinlock_t g_dev_lock;
static int g_initialized = 0;

/* ── Legacy adapter ───────────────────────────────────────────────── */
/* Wraps a pair of read_fn/write_fn into a submit_fn for backward compat */

struct legacy_adapter {
    blockdev_read_fn  read_fn;
    blockdev_write_fn write_fn;
};

static int legacy_submit_fn(struct blk_request *req) {
    struct legacy_adapter *ad = (struct legacy_adapter *)req->buf; /* not used */
    (void)ad;
    return -1; /* shouldn't be called directly — we handle sync inline */
}

/* ── Request slab cache ───────────────────────────────────────────── */

struct blk_request *blk_request_alloc(void) {
    struct blk_request *req = (struct blk_request *)kmalloc(sizeof(struct blk_request));
    if (!req) return NULL;
    memset(req, 0, sizeof(*req));
    return req;
}

void blk_request_free(struct blk_request *req) {
    if (req) kfree(req);
}

/* ── Request queue operations ─────────────────────────────────────── */

static void queue_insert(struct blk_request_queue *q, struct blk_request *req) {
    q->queued_count++;

    if (q->sched == BLK_SCHED_NOOP || !q->head) {
        /* NOOP: append to tail (FIFO) */
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

    /* Deadline scheduler: insert in expiry order, reads before writes */
    if (q->sched == BLK_SCHED_DEADLINE) {
        req->expiry = timer_get_ticks() + 5; /* 50ms deadline at 100Hz */
        struct blk_request **pp = &q->head;
        while (*pp) {
            int insert_here = 0;
            if (req->flags & BLK_REQ_READ && !((*pp)->flags & BLK_REQ_READ)) {
                insert_here = (req->expiry <= (*pp)->expiry + 2);
            } else {
                insert_here = (req->expiry < (*pp)->expiry) ||
                              (req->expiry == (*pp)->expiry && req->lba < (*pp)->lba);
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

    /* If no requests in flight and queue was empty, dispatch directly */
    if (q->inflight_count == 0 && q->queued_count == 0) {
        req->inflight = 1;
        q->inflight_count++;
        spinlock_irqsave_release(&q->lock, irq_flags);

        int ret = g_blockdevs[dev_id].submit_fn(req);
        /* Synchronous drivers complete the request during submit_fn.
         * Mark done and wake waiter on success or failure. */
        q->inflight_count--;
        req->done = 1;
        if (req->done_wq) {
            wait_queue_wake(req->done_wq);
        }
        return ret < 0 ? ret : 0;
    }

    /* Queue for later dispatch */
    queue_insert(q, req);
    spinlock_irqsave_release(&q->lock, irq_flags);
    return 0;
}

int blk_submit_sync(int dev_id, uint64_t lba, uint32_t count,
                    void *buf, uint32_t flags) {
    if (!g_initialized) return -1;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;

    struct blk_request *req = blk_request_alloc();
    if (!req) return -6; /* ENOMEM */

    req->dev_id = dev_id;
    req->lba = lba;
    req->count = count;
    req->buf = buf;
    req->flags = flags;

    /* Synchronous: use a waitqueue on the stack */
    struct wait_queue wq;
    wait_queue_init(&wq);
    req->done_wq = &wq;

    int ret = blk_submit_async(req);
    if (ret < 0) {
        blk_request_free(req);
        return ret;
    }

    /* Wait for completion */
    while (!req->done) {
        wait_queue_sleep(&wq);
    }

    ret = req->result;
    blk_request_free(req);
    return ret;
}

/* Driver calls this when an async request completes */
void blk_request_done(struct blk_request *req) {
    if (!req) return;

    req->done = 1;
    req->inflight = 0;

    struct blk_request_queue *q = &g_queues[req->dev_id];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&q->lock, &irq_flags);
    q->inflight_count--;
    spinlock_irqsave_release(&q->lock, irq_flags);

    /* Wake synchronous waiter */
    if (req->done_wq) {
        wait_queue_wake(req->done_wq);
    }
}

/* Dequeue the next request for the driver to process.
 * Returns 1 if a request was dequeued, 0 if queue empty. */
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

/* Deadline scheduler: peek the next request with read priority */
struct blk_request *blk_request_deadline_peek(struct blk_request_queue *q) {
    if (!q) return NULL;
    return queue_peek(q);
}

/* ── Release all resources for a driver's queue ───────────────────── */

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
                      uint64_t sector_count) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    if (!submit_fn) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_dev_lock, &irq_flags);

    /* Drain any stale queue (re-registration) */
    if (g_blockdevs[id].active) {
        drain_queue(&g_queues[id]);
    }

    g_blockdevs[id].active = 1;
    g_blockdevs[id].submit_fn = submit_fn;
    g_blockdevs[id].idle_fn = idle_fn;
    g_blockdevs[id].sector_count = sector_count;
    g_blockdevs[id].sector_size = 512;

    if (name && *name) {
        strncpy(g_blockdevs[id].name, name, sizeof(g_blockdevs[id].name) - 1);
        g_blockdevs[id].name[sizeof(g_blockdevs[id].name) - 1] = '\0';
    } else {
        g_blockdevs[id].name[0] = '\0';
    }

    /* Initialize request queue */
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
/* Legacy registration: adapts old read/write/size functions to new API. */
struct legacy_driver {
    blockdev_read_fn  read_fn;
    blockdev_write_fn write_fn;
};

static struct legacy_driver g_legacy[BLOCKDEV_MAX_DEVICES];

static int legacy_submit_fn_adapter(struct blk_request *req) {
    int dev_id = req->dev_id;
    if (dev_id < 0 || dev_id >= BLOCKDEV_MAX_DEVICES || !g_blockdevs[dev_id].active)
        return -1;

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
    return req->result;
}

int blockdev_register_legacy(int id, const char *name,
                              blockdev_read_fn read_fn,
                              blockdev_write_fn write_fn,
                              blockdev_size_fn size_fn) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return -1;
    if (!read_fn) return -1;

    /* Store legacy callbacks */
    g_legacy[id].read_fn = read_fn;
    g_legacy[id].write_fn = write_fn;

    uint64_t nsectors = size_fn ? size_fn() : 0;

    /* Register with the new API using our adapter submit_fn */
    int ret = blockdev_register(id, name, legacy_submit_fn_adapter, NULL, nsectors);
    if (ret < 0) return ret;

    return 0;
}
#endif

int blockdev_is_registered(int id) {
    if (id < 0 || id >= BLOCKDEV_MAX_DEVICES) return 0;
    return g_blockdevs[id].active;
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
