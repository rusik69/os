/*
 * iosched.c — Block I/O Scheduler (Elevator) — B15
 *
 * Implements three scheduling policies:
 *   NOOP     — simple FIFO (default for SSD / NVMe)
 *   DEADLINE — read-biased with per-request deadlines
 *   CFQ      — Complete Fair Queueing with per-process queues
 *
 * Integrates with the existing blockdev / request infrastructure.
 * Each block device has an associated struct iosched_queue that
 * manages request ordering, merging, and dispatch.
 */

#define KERNEL_INTERNAL
#include "iosched.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "timer.h"
#include "export.h"
#include "errno.h"
#include "process.h"
#include "module.h"
#include "hrtimer.h"
#include "ioprio.h"

/* ── Per-device iosched queue table ───────────────────────────────── */

#define IOSCHED_MAX_DEVICES  BLOCKDEV_MAX_DEVICES

static struct iosched_queue g_iosched_queues[IOSCHED_MAX_DEVICES];
static int g_iosched_initialized = 0;

/* ── Forward declarations of policy operations ───────────────────── */

static int  noop_submit(struct iosched_queue *iq, struct blk_request *req);
static struct blk_request *noop_fetch(struct iosched_queue *iq);
static void noop_free(struct iosched_queue *iq);

static int  deadline_submit(struct iosched_queue *iq, struct blk_request *req);
static struct blk_request *deadline_fetch(struct iosched_queue *iq);
static void deadline_free(struct iosched_queue *iq);

static int  cfq_submit(struct iosched_queue *iq, struct blk_request *req);
static struct blk_request *cfq_fetch(struct iosched_queue *iq);
static void cfq_free(struct iosched_queue *iq);

static int  kyber_submit(struct iosched_queue *iq, struct blk_request *req);
static struct blk_request *kyber_fetch(struct iosched_queue *iq);
static void kyber_free(struct iosched_queue *iq);

static const struct iosched_ops g_noop_ops = {
    .name   = "noop",
    .submit = noop_submit,
    .fetch  = noop_fetch,
    .free   = noop_free,
};

static const struct iosched_ops g_deadline_ops = {
    .name   = "deadline",
    .submit = deadline_submit,
    .fetch  = deadline_fetch,
    .free   = deadline_free,
};

static const struct iosched_ops g_cfq_ops = {
    .name   = "cfq",
    .submit = cfq_submit,
    .fetch  = cfq_fetch,
    .free   = cfq_free,
};

static const struct iosched_ops g_kyber_ops = {
    .name   = "kyber",
    .submit = kyber_submit,
    .fetch  = kyber_fetch,
    .free   = kyber_free,
};

/* ── Helpers ─────────────────────────────────────────────────────── */

static inline int is_read(struct blk_request *req)
{
    return (req->flags & BLK_REQ_READ) != 0;
}

static inline int same_dir(struct blk_request *a, struct blk_request *b)
{
    return ((a->flags ^ b->flags) & (BLK_REQ_READ | BLK_REQ_WRITE)) == 0;
}

/* ── iosched_init ────────────────────────────────────────────────── */

void iosched_init(void)
{
    memset(g_iosched_queues, 0, sizeof(g_iosched_queues));
    for (int i = 0; i < IOSCHED_MAX_DEVICES; i++) {
        spinlock_init(&g_iosched_queues[i].lock);
        g_iosched_queues[i].dev_id = i;
        g_iosched_queues[i].policy = IOSCHED_NOOP;
        g_iosched_queues[i].ops    = &g_noop_ops;
    }
    g_iosched_initialized = 1;
    kprintf("[OK] iosched: I/O scheduler initialized (NOOP/DEADLINE/CFQ/KYBER)\n");
}

/* ── iosched_get_queue ───────────────────────────────────────────── */

struct iosched_queue *iosched_get_queue(int dev_id)
{
    if (!g_iosched_initialized) return NULL;
    if (dev_id < 0 || dev_id >= IOSCHED_MAX_DEVICES) return NULL;
    return &g_iosched_queues[dev_id];
}

/* ── iosched_submit_request ──────────────────────────────────────── */

int iosched_submit_request(int dev_id, struct blk_request *req)
{
    struct iosched_queue *iq = iosched_get_queue(dev_id);
    if (!iq || !req) return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&iq->lock, &irq_flags);

    req->next = NULL;
    req->inflight = 0;

    int ret = iq->ops->submit(iq, req);

    spinlock_irqsave_release(&iq->lock, irq_flags);
    return ret;
}

/* ── iosched_fetch_request ───────────────────────────────────────── */

struct blk_request *iosched_fetch_request(int dev_id)
{
    struct iosched_queue *iq = iosched_get_queue(dev_id);
    if (!iq) return NULL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&iq->lock, &irq_flags);

    struct blk_request *req = iq->ops->fetch(iq);

    spinlock_irqsave_release(&iq->lock, irq_flags);
    return req;
}

/* ── iosched_set_policy / get_policy ─────────────────────────────── */

int iosched_set_policy(int dev_id, int policy)
{
    struct iosched_queue *iq = iosched_get_queue(dev_id);
    if (!iq) return -ENODEV;
    if (policy != IOSCHED_NOOP && policy != IOSCHED_DEADLINE && policy != IOSCHED_CFQ && policy != IOSCHED_KYBER)
        return -EINVAL;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&iq->lock, &irq_flags);

    /* Free existing policy data */
    if (iq->ops && iq->ops->free)
        iq->ops->free(iq);

    /* Reset request list */
    iq->head = NULL;
    iq->tail = NULL;
    iq->queued_count = 0;

    /* Select new policy */
    iq->policy = policy;
    switch (policy) {
    case IOSCHED_NOOP:
        iq->ops = &g_noop_ops;
        memset(&iq->noop, 0, sizeof(iq->noop));
        break;
    case IOSCHED_DEADLINE:
        iq->ops = &g_deadline_ops;
        memset(&iq->deadline, 0, sizeof(iq->deadline));
        break;
    case IOSCHED_CFQ:
        iq->ops = &g_cfq_ops;
        memset(&iq->cfq, 0, sizeof(iq->cfq));
        INIT_LIST_HEAD(&iq->cfq.rt_queues);
        INIT_LIST_HEAD(&iq->cfq.be_queues);
        INIT_LIST_HEAD(&iq->cfq.idle_queues);
        break;
    case IOSCHED_KYBER:
        iq->ops = &g_kyber_ops;
        memset(&iq->kyber, 0, sizeof(iq->kyber));
        /* Initialize Kyber domain token budgets */
        iq->kyber.domains[KYBER_READ_DOMAIN].tokens    = KYBER_READ_TOKENS_MAX;
        iq->kyber.domains[KYBER_READ_DOMAIN].tokens_min = KYBER_READ_TOKENS_MIN;
        iq->kyber.domains[KYBER_READ_DOMAIN].tokens_max = KYBER_READ_TOKENS_MAX;
        iq->kyber.domains[KYBER_READ_DOMAIN].target_ns  = KYBER_READ_TARGET_NS;
        iq->kyber.domains[KYBER_WRITE_DOMAIN].tokens    = KYBER_WRITE_TOKENS_MAX;
        iq->kyber.domains[KYBER_WRITE_DOMAIN].tokens_min = KYBER_WRITE_TOKENS_MIN;
        iq->kyber.domains[KYBER_WRITE_DOMAIN].tokens_max = KYBER_WRITE_TOKENS_MAX;
        iq->kyber.domains[KYBER_WRITE_DOMAIN].target_ns  = KYBER_WRITE_TARGET_NS;
        iq->kyber.domains[KYBER_DISCARD_DOMAIN].tokens    = KYBER_DISCARD_TOKENS_MAX;
        iq->kyber.domains[KYBER_DISCARD_DOMAIN].tokens_min = KYBER_DISCARD_TOKENS_MIN;
        iq->kyber.domains[KYBER_DISCARD_DOMAIN].tokens_max = KYBER_DISCARD_TOKENS_MAX;
        iq->kyber.domains[KYBER_DISCARD_DOMAIN].target_ns  = KYBER_DISCARD_TARGET_NS;
        iq->kyber.domains[KYBER_OTHER_DOMAIN].tokens    = KYBER_OTHER_TOKENS_MAX;
        iq->kyber.domains[KYBER_OTHER_DOMAIN].tokens_min = KYBER_OTHER_TOKENS_MIN;
        iq->kyber.domains[KYBER_OTHER_DOMAIN].tokens_max = KYBER_OTHER_TOKENS_MAX;
        iq->kyber.domains[KYBER_OTHER_DOMAIN].target_ns  = KYBER_OTHER_TARGET_NS;
        break;
    default:
        iq->ops = &g_noop_ops;
        break;
    }

    spinlock_irqsave_release(&iq->lock, irq_flags);
    return 0;
}

int iosched_get_policy(int dev_id)
{
    struct iosched_queue *iq = iosched_get_queue(dev_id);
    if (!iq) return -ENODEV;
    return iq->policy;
}

/* ── iosched_request_complete ────────────────────────────────────── */

/* Forward declaration for Kyber latency tracking */
static void kyber_record_latency(struct iosched_kyber_data *kd,
                                  struct blk_request *req);

void iosched_request_complete(int dev_id, struct blk_request *req)
{
    struct iosched_queue *iq = iosched_get_queue(dev_id);
    if (!iq || !req) return;

    /* Kyber uses completion to track I/O latency for token adjustment */
    if (iq->policy == IOSCHED_KYBER) {
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&iq->lock, &irq_flags);
        kyber_record_latency(&iq->kyber, req);
        spinlock_irqsave_release(&iq->lock, irq_flags);
    }
    /* CFQ uses completion to re-arm slices */
}

/* ════════════════════════════════════════════════════════════════════
 * NOOP scheduler (FIFO)
 *
 * The NOOP scheduler maintains a simple FIFO queue with full back-merge
 * and front-merge support.  On submission the scheduler scans the queue
 * for merge opportunities:
 *   1. Fast path: back-merge with tail (most common)
 *   2. Fast path: front-merge with head
 *   3. Slow path: walk the full queue looking for any merge candidate
 *   4. Fallback: append to tail
 *
 * This approach captures the maximum number of merge opportunities while
 * maintaining O(n) worst-case (which is acceptable because NOOP targets
 * SSDs / NVMe where queue depths are bounded).
 * ════════════════════════════════════════════════════════════════════ */

static int noop_submit(struct iosched_queue *iq, struct blk_request *req)
{
    /* ── 1. Fast path: back-merge with tail ── */
    if (iq->tail && same_dir(iq->tail, req) &&
        iq->tail->lba + iq->tail->count == req->lba) {
        iq->tail->count += req->count;
        iq->noop.back_merges++;
        iq->noop.total_merges++;
        return 0;
    }

    /* ── 2. Fast path: front-merge with head ── */
    if (iq->head && same_dir(iq->head, req) &&
        req->lba + req->count == iq->head->lba) {
        iq->head->lba = req->lba;
        iq->head->count += req->count;
        iq->noop.front_merges++;
        iq->noop.total_merges++;
        return 0;
    }

    /* ── 3. Full queue scan for mid-queue merges ── */
    struct blk_request *cur = iq->head;
    while (cur) {
        if (same_dir(cur, req)) {
            /* Back merge: cur immediately precedes req */
            if (cur->lba + cur->count == req->lba) {
                cur->count += req->count;
                iq->noop.back_merges++;
                iq->noop.total_merges++;
                return 0;
            }
            /* Front merge: req immediately precedes cur */
            if (req->lba + req->count == cur->lba) {
                cur->lba = req->lba;
                cur->count += req->count;
                iq->noop.front_merges++;
                iq->noop.total_merges++;
                return 0;
            }
        }
        cur = cur->next;
    }

    /* ── 4. No merge found — append to tail ── */
    if (iq->tail) {
        iq->tail->next = req;
        iq->tail = req;
    } else {
        iq->head = req;
        iq->tail = req;
    }
    req->next = NULL;
    iq->queued_count++;
    iq->noop.submitted++;
    return 0;
}

static struct blk_request *noop_fetch(struct iosched_queue *iq)
{
    struct blk_request *req = iq->head;
    if (!req) return NULL;

    iq->head = req->next;
    if (!iq->head) iq->tail = NULL;
    req->next = NULL;
    iq->queued_count--;
    iq->noop.fetched++;
    return req;
}

static void noop_free(struct iosched_queue *iq)
{
    memset(&iq->noop, 0, sizeof(iq->noop));
}

/* ════════════════════════════════════════════════════════════════════
 * Deadline scheduler — read-biased with per-request deadlines
 *
 * Dispatch algorithm (enhanced with fifo_batch batching):
 *   1. Check FIFO expiry lists — drain any expired requests first
 *   2. Determine current dispatch direction via current_queue
 *   3. Dispatch up to DEADLINE_FIFO_BATCH (16) requests in that direction
 *   4. After a batch, check write starvation — if starved >= STARVE_LIMIT,
 *      switch to writes (serve a batch of writes)
 *   5. On direction switch, increment batches counter, reset starved as
 *      appropriate, and note last_tick for accounting
 *   6. Request sorting by LBA within each direction queue enables sector
 *      ordering for better disk head movement
 * ════════════════════════════════════════════════════════════════════ */

/* ── Internal helpers ──────────────────────────────────────────── */

static void deadline_enqueue_fifo(struct iosched_deadline_data *dd,
                                   struct blk_request *req, int qid)
{
    /* Insert into FIFO list sorted by expiry (earliest first) */
    struct blk_request **pp = &dd->fifo_list[qid];
    while (*pp) {
        if (req->expiry < (*pp)->expiry)
            break;
        pp = &(*pp)->next;
    }
    req->next = *pp;
    *pp = req;
    if (!req->next)
        dd->fifo_tail[qid] = req;
    dd->fifo_count[qid]++;
}

static struct blk_request *deadline_dequeue_fifo(struct iosched_deadline_data *dd,
                                                  int qid)
{
    struct blk_request *req = dd->fifo_list[qid];
    if (!req) return NULL;
    dd->fifo_list[qid] = req->next;
    if (!dd->fifo_list[qid])
        dd->fifo_tail[qid] = NULL;
    req->next = NULL;
    dd->fifo_count[qid]--;
    return req;
}

/* Remove a request from the LBA-sorted queue for the given direction.
 * Returns the request or NULL if not found. */
static struct blk_request *deadline_remove_from_sorted(
    struct iosched_deadline_data *dd, struct blk_request *req, int qid)
{
    struct blk_request **pp = &dd->queues[qid].head;
    while (*pp) {
        if (*pp == req) {
            *pp = req->next;
            if (!req->next)
                dd->queues[qid].tail = NULL;
            req->next = NULL;
            dd->queues[qid].count--;
            return req;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Drain all expired requests from a direction's FIFO list.
 * Returns the first expired request found (or NULL if none expired).
 * Expired requests are removed from BOTH the FIFO list and the sorted queue. */
static struct blk_request *deadline_drain_expired(
    struct iosched_deadline_data *dd, int qid, uint64_t now)
{
    struct blk_request *fifo = dd->fifo_list[qid];
    while (fifo && fifo->expiry <= now) {
        struct blk_request *expired = deadline_dequeue_fifo(dd, qid);
        deadline_remove_from_sorted(dd, expired, qid);
        expired->next = NULL;
        dd->expired++;
        return expired;
    }
    return NULL;
}

/* Dispatch one request from the LBA-sorted queue for the given direction.
 * Also removes the request from the FIFO list.
 * Returns the request or NULL if the queue is empty. */
static struct blk_request *deadline_dispatch_from_queue(
    struct iosched_deadline_data *dd, int qid)
{
    struct blk_request *req = dd->queues[qid].head;
    if (!req) return NULL;

    /* Remove from sorted queue */
    dd->queues[qid].head = req->next;
    if (!req->next)
        dd->queues[qid].tail = NULL;
    req->next = NULL;
    dd->queues[qid].count--;

    /* Remove from FIFO list */
    struct blk_request **pp = &dd->fifo_list[qid];
    while (*pp && *pp != req)
        pp = &(*pp)->next;
    if (*pp == req) {
        *pp = req->next;
        if (!req->next)
            dd->fifo_tail[qid] = NULL;
        req->next = NULL;
        dd->fifo_count[qid]--;
    }

    return req;
}

/* ── deadline_submit ───────────────────────────────────────────── */

static int deadline_submit(struct iosched_queue *iq, struct blk_request *req)
{
    struct iosched_deadline_data *dd = &iq->deadline;
    uint64_t now = timer_get_ms();
    int qid;

    /* Set deadline: reads 500ms, writes 5s */
    if (is_read(req)) {
        req->expiry = now + DEADLINE_READ_MS;
        qid = DD_READ_QUEUE;
    } else {
        req->expiry = now + DEADLINE_WRITE_MS;
        qid = DD_WRITE_QUEUE;
    }

    dd->submitted++;

    /* Try to merge with existing requests in the per-direction queue */
    struct blk_request *cur = dd->queues[qid].head;
    while (cur) {
        if (same_dir(cur, req)) {
            /* Back merge: cur immediately precedes req */
            if (cur->lba + cur->count == req->lba) {
                cur->count += req->count;
                dd->back_merges++;
                dd->total_merges++;
                return 0;
            }
            /* Front merge: req immediately precedes cur */
            if (req->lba + req->count == cur->lba) {
                req->count += cur->count;
                /* Remove cur from sorted queue */
                deadline_remove_from_sorted(dd, cur, qid);
                /* Remove cur from FIFO list */
                struct blk_request **fp = &dd->fifo_list[qid];
                while (*fp && *fp != cur)
                    fp = &(*fp)->next;
                if (*fp == cur) {
                    *fp = cur->next;
                    if (!cur->next)
                        dd->fifo_tail[qid] = NULL;
                    cur->next = NULL;
                    dd->fifo_count[qid]--;
                }
                blk_request_free(cur);
                dd->front_merges++;
                dd->total_merges++;
                /* Fall through to insert the merged request */
                break;
            }
        }
        cur = cur->next;
    }

    /* Insert into the per-direction sorted queue (sort by LBA for
     * better disk head movement and merge opportunities) */
    struct blk_request **pp = &dd->queues[qid].head;
    while (*pp) {
        if (req->lba < (*pp)->lba)
            break;
        pp = &(*pp)->next;
    }
    req->next = *pp;
    *pp = req;
    if (!req->next)
        dd->queues[qid].tail = req;
    dd->queues[qid].count++;

    /* Also add to FIFO list for deadline enforcement */
    deadline_enqueue_fifo(dd, req, qid);

    return 0;
}

/* ── deadline_fetch ────────────────────────────────────────────── */

static struct blk_request *deadline_fetch(struct iosched_queue *iq)
{
    struct iosched_deadline_data *dd = &iq->deadline;
    uint64_t now = timer_get_ms();
    struct blk_request *req;
    int qid;

    /* Phase 1: Check for expired requests in BOTH queues.
     * Expired requests get absolute priority — return the first one found. */
    req = deadline_drain_expired(dd, DD_READ_QUEUE, now);
    if (req) return req;

    req = deadline_drain_expired(dd, DD_WRITE_QUEUE, now);
    if (req) return req;

    /* Phase 2: Determine dispatch direction with fifo_batch batching.
     *
     * The algorithm alternates between read and write directions
     * in batches of DEADLINE_FIFO_BATCH (16) requests. After each
     * batch completes, we check write starvation — if READS have been
     * dispatched for DEADLINE_STARVE_LIMIT batches without WRITES,
     * the next batch is forced to WRITE. */

    if (dd->queues[DD_WRITE_QUEUE].count == 0 &&
        dd->queues[DD_READ_QUEUE].count == 0)
        return NULL;

    /* Determine which direction to dispatch */
    if (dd->queues[DD_READ_QUEUE].count == 0) {
        /* No reads — must serve writes */
        qid = DD_WRITE_QUEUE;
    } else if (dd->queues[DD_WRITE_QUEUE].count == 0) {
        /* No writes — serve reads */
        qid = DD_READ_QUEUE;
    } else {
        /* Both queues have requests — apply batching policy */

        /* If current_queue is uninitialized, start with reads */
        if (dd->current_queue != DD_READ_QUEUE &&
            dd->current_queue != DD_WRITE_QUEUE) {
            dd->current_queue = DD_READ_QUEUE;
            dd->batches = 0;
            dd->last_tick = now;
        }

        /* Check write starvation: if reads have dominated for too many
         * batches, force a write batch */
        if (dd->current_queue == DD_READ_QUEUE &&
            dd->starved >= DEADLINE_STARVE_LIMIT &&
            dd->queues[DD_WRITE_QUEUE].count > 0) {
            /* Switch to writes — serve a batch */
            dd->current_queue = DD_WRITE_QUEUE;
            dd->batches = 0;
            dd->starved = 0;
            dd->last_tick = now;
        }

        if (dd->current_queue == DD_WRITE_QUEUE) {
            /* In a write batch — check if batch is done */
            if (dd->batches >= DEADLINE_FIFO_BATCH ||
                dd->queues[DD_WRITE_QUEUE].count == 0) {
                /* Switch back to reads */
                if (dd->queues[DD_READ_QUEUE].count > 0) {
                    dd->current_queue = DD_READ_QUEUE;
                    dd->batches = 0;
                    dd->starved = 0;
                    dd->last_tick = now;
                }
            }
        }

        /* Prefer current direction */
        qid = dd->current_queue;

        /* If the preferred queue is empty, fall back to the other */
        if (dd->queues[qid].count == 0) {
            qid = (qid == DD_READ_QUEUE) ? DD_WRITE_QUEUE : DD_READ_QUEUE;
            dd->current_queue = qid;
            dd->batches = 0;
            dd->starved = 0;
            dd->last_tick = now;
        }
    }

    /* Dispatch one request from the chosen queue */
    req = deadline_dispatch_from_queue(dd, qid);
    if (!req)
        return NULL;

    dd->fetched++;

    /* Track batching */
    if (qid == DD_READ_QUEUE) {
        dd->batches++;
        /* Only starve other direction if reads keep coming */
        if (dd->queues[DD_READ_QUEUE].count > 0)
            dd->starved++;
    } else {
        dd->batches++;
        if (dd->queues[DD_WRITE_QUEUE].count > 0)
            dd->starved = 0; /* Reset starved on write dispatch */
    }

    return req;
}

/* ── deadline_free ─────────────────────────────────────────────── */

static void deadline_free(struct iosched_queue *iq)
{
    /* Deadline data is inline in the iosched_queue union — just zero it */
    memset(&iq->deadline, 0, sizeof(iq->deadline));
}

/* ════════════════════════════════════════════════════════════════════
 * CFQ scheduler (Complete Fair Queueing)
 *
 * Dispatch algorithm:
 *   1. Select priority class: RT (real-time) → BE (best-effort) → IDLE
 *   2. Within each class, round-robin between per-process queues
 *   3. Each queue gets a priority-weighted time slice; lower ioprio
 *      (within class) yields a proportionally larger slice
 *   4. After dispatching from a queue that serves synchronous I/O,
 *      if the queue empties, idle for CFQ_IDLE_DELAY_NS (8ms)
 *      waiting for more requests from the same process (anticipatory
 *      scheduling).  If the process submits more within the idle
 *      window, serve them immediately — otherwise move on.
 *   5. Write starvation: if read dispatch dominates for more than
 *      CFQ_WRITE_STARVE_NS, force a write queue next.
 *   6. IDLE-class queues only dispatch when no RT or BE work exists.
 * ════════════════════════════════════════════════════════════════════ */

/* ── Idle timer callback ───────────────────────────────────────── */

static void cfq_idle_timer_cb(void *arg)
{
    struct iosched_queue *iq = (struct iosched_queue *)arg;
    iq->cfq.idle_expired = 1;
}

/* ── Helper: get the class list for a given priority class ──────── */

static struct list_head *cfq_class_list(struct iosched_cfq_data *cfq,
                                         unsigned int ioprio_class)
{
    switch (ioprio_class) {
    case IOPRIO_CLASS_RT:
        return &cfq->rt_queues;
    case IOPRIO_CLASS_BE:
        return &cfq->be_queues;
    case IOPRIO_CLASS_IDLE:
        return &cfq->idle_queues;
    default:
        return &cfq->be_queues;
    }
}

/* ── Helper: priority weight for time slice scaling ─────────────── */

static uint32_t cfq_class_weight(unsigned int ioprio_class,
                                  unsigned int ioprio_data)
{
    switch (ioprio_class) {
    case IOPRIO_CLASS_RT:
        return CFQ_WEIGHT_RT;
    case IOPRIO_CLASS_BE:
        /* Scale BE weight by priority level (0=highest=more weight) */
        return CFQ_WEIGHT_BE_DEF * (8 - (ioprio_data & 0x7));
    case IOPRIO_CLASS_IDLE:
        return CFQ_WEIGHT_IDLE;
    default:
        return CFQ_WEIGHT_BE_DEF;
    }
}

/* ── Helper: is request synchronous? ────────────────────────────── */

static inline int cfq_req_is_sync(struct blk_request *req)
{
    return (req->flags & BLK_REQ_SYNC) != 0;
}

/* ── Find or create a per-process queue ─────────────────────────── */

static struct cfq_queue *cfq_get_queue(struct iosched_cfq_data *cfq,
                                        struct blk_request *req)
{
    struct process *proc = process_get_current();
    uint64_t pid = proc ? (uint64_t)(proc->pid) : 0;

    /* Look for existing queue for this PID */
    for (int i = 0; i < cfq->queue_count; i++) {
        if (cfq->queues[i].pid == pid && cfq->queues[i].count >= 0)
            return &cfq->queues[i];
    }

    /* Create a new queue if space allows */
    if (cfq->queue_count >= CFQ_QUEUES_MAX)
        return NULL;

    struct cfq_queue *q = &cfq->queues[cfq->queue_count];
    memset(q, 0, sizeof(*q));
    q->pid = pid;
    q->count = 0;
    q->dispatched = 0;
    q->ioprio = req->ioprio;
    q->is_sync = cfq_req_is_sync(req);
    /* Compute priority-weighted slice quota */
    unsigned int cls = IOPRIO_PRIO_CLASS(req->ioprio);
    unsigned int dat = IOPRIO_PRIO_DATA(req->ioprio);
    q->slice_quota = CFQ_SLICE_MS * cfq_class_weight(cls, dat) / CFQ_WEIGHT_BE_DEF;
    if (q->slice_quota < 10)
        q->slice_quota = 10;  /* minimum 10ms slice */
    cfq->queue_count++;

    return q;
}

/* ── cfq_submit — submit a request to CFQ ──────────────────────── */

static int cfq_submit(struct iosched_queue *iq, struct blk_request *req)
{
    struct iosched_cfq_data *cfq = &iq->cfq;

    struct cfq_queue *q = cfq_get_queue(cfq, req);
    if (!q) {
        /* Fall back to queue 0 if max queues reached */
        q = &cfq->queues[0];
        if (!q) return -ENOMEM;
    }

    /* Update sync/async tracking */
    if (cfq_req_is_sync(req))
        q->is_sync = 1;

    /* If we are idling for this queue and the process submitted more,
     * cancel the idle timer — we can serve this immediately. */
    if (cfq->idle_q == q && q->count == 0 &&
        hrtimer_active(&cfq->idle_timer)) {
        hrtimer_cancel(&cfq->idle_timer);
        cfq->idle_expired = 0;
        cfq->idle_q = NULL;
        cfq->idle_hits++;
    }

    /* Try to merge with the last request in this per-process queue */
    if (q->tail && q->tail->lba + q->tail->count == req->lba &&
        ((q->tail->flags ^ req->flags) & (BLK_REQ_READ | BLK_REQ_WRITE)) == 0) {
        q->tail->count += req->count;
        return 0;
    }

    /* Append to per-process queue */
    if (q->tail) {
        q->tail->next = req;
        q->tail = req;
    } else {
        q->head = req;
        q->tail = req;
    }
    req->next = NULL;
    q->count++;

    /* Add to appropriate class list if this is the first request */
    if (q->count == 1 && list_empty(&q->list)) {
        unsigned int cls = IOPRIO_PRIO_CLASS(q->ioprio);
        if (cls == 0) cls = IOPRIO_CLASS_BE; /* NONE → BE */
        list_add_tail(&q->list, cfq_class_list(cfq, cls));
    }

    return 0;
}

/* ── Helper: pick the next priority class that has work ─────────── */

static int cfq_pick_class(struct iosched_cfq_data *cfq)
{
    if (!list_empty(&cfq->rt_queues))
        return IOPRIO_CLASS_RT;
    if (!list_empty(&cfq->be_queues))
        return IOPRIO_CLASS_BE;
    if (!list_empty(&cfq->idle_queues))
        return IOPRIO_CLASS_IDLE;
    return -1;
}

/* ── cfq_fetch — fetch the next request from CFQ ───────────────── */

static struct blk_request *cfq_fetch(struct iosched_queue *iq)
{
    struct iosched_cfq_data *cfq = &iq->cfq;
    uint64_t now = timer_get_ms();

    /* ── Phase 1: Check idle timer state ── */
    if (cfq->idle_q && cfq->idle_expired) {
        /* Idle timed out — move off this queue */
        cfq->idle_q = NULL;
        cfq->idle_expired = 0;
        cfq->current_q = NULL;
    }

    if (cfq->idle_q) {
        /* Still within the idle window — check if the queue has requests */
        struct cfq_queue *iqidle = cfq->idle_q;
        if (iqidle->head) {
            /* Process submitted more during idle window — serve it */
            hrtimer_cancel(&cfq->idle_timer);
            cfq->idle_expired = 0;
            struct blk_request *req = iqidle->head;
            iqidle->head = req->next;
            if (!iqidle->head)
                iqidle->tail = NULL;
            req->next = NULL;
            iqidle->count--;
            iqidle->dispatched++;
            if (iqidle->count > 0)
                iqidle->last_fetch = timer_get_ns();
            cfq->current_q = iqidle;
            return req;
        }
        /* Queue is still empty during idle window — return NULL,
         * the caller will retry or a new submission will trigger. */
        return NULL;
    }

    /* ── Phase 2: Check current queue — is its slice still valid? ── */
    if (cfq->current_q) {
        uint64_t elapsed = now - cfq->current_q->slice_start;

        /* Allow a bit of slack (10% of quota) before switching */
        uint32_t slack = cfq->current_q->slice_quota / 10;
        if (slack < 5) slack = 5;

        if (elapsed >= cfq->current_q->slice_quota + slack ||
            cfq->current_q->count == 0) {

            /* Slice expired or queue empty — move to next */
            struct cfq_queue *old_q = cfq->current_q;

            if (old_q->count > 0) {
                /* Still has requests but slice expired — re-queue */
                unsigned int cls = IOPRIO_PRIO_CLASS(old_q->ioprio);
                if (cls == 0) cls = IOPRIO_CLASS_BE;
                list_add_tail(&old_q->list, cfq_class_list(cfq, cls));
            } else {
                /* Queue empty — check if we should idle */
                if (old_q->is_sync && old_q->dispatched > 0) {
                    /* Start anticipatory idle — wait for more I/O */
                    cfq->idle_q = old_q;
                    cfq->idle_expired = 0;
                    hrtimer_init(&cfq->idle_timer, cfq_idle_timer_cb, iq);
                    hrtimer_start(&cfq->idle_timer, CFQ_IDLE_DELAY_NS);
                    cfq->idle_waits++;
                    /* Current queue becomes the idle one */
                    cfq->current_q = old_q;
                    return NULL;
                }
                /* Not sync — remove from active list */
                INIT_LIST_HEAD(&old_q->list);
            }
            cfq->current_q = NULL;
        } else {
            /* Slice still valid — dispatch from current queue */
            struct cfq_queue *q = cfq->current_q;
            if (!q || !q->head)
                return NULL;

            struct blk_request *req = q->head;
            q->head = req->next;
            if (!q->head)
                q->tail = NULL;
            req->next = NULL;
            q->count--;
            q->dispatched++;
            q->last_fetch = timer_get_ns();

            /* Track sync/async */
            if (cfq_req_is_sync(req))
                q->is_sync = 1;

            return req;
        }
    }

    /* ── Phase 3: Select next priority class and queue ── */

    /* Check write starvation: if reads have dominated for too long,
     * force a write dispatch by temporarily ignoring reads. */
    if (!list_empty(&cfq->be_queues) &&
        cfq->read_dispatched > 64) {
        uint64_t now_ns = timer_get_ns();
        if (now_ns - cfq->last_write_tick >= CFQ_WRITE_STARVE_NS) {
            /* Force write dispatch: scan BE queues for writes */
            struct list_head *pos;
            list_for_each(pos, &cfq->be_queues) {
                struct cfq_queue *wq = list_entry(pos, struct cfq_queue, list);
                if (wq->head &&
                    (wq->head->flags & BLK_REQ_WRITE)) {
                    /* Found a write queue — serve it now */
                    list_del(&wq->list);
                    INIT_LIST_HEAD(&wq->list);
                    wq->slice_start = now;
                    wq->dispatched = 0;
                    cfq->current_q = wq;
                    cfq->queue_starved++;
                    cfq->read_dispatched = 0;
                    cfq->last_write_tick = now_ns;

                    struct blk_request *req = wq->head;
                    wq->head = req->next;
                    if (!wq->head)
                        wq->tail = NULL;
                    req->next = NULL;
                    wq->count--;
                    wq->dispatched++;
                    wq->last_fetch = timer_get_ns();
                    return req;
                }
            }
        }
    }

    /* Pick the next priority class with pending work */
    int cls = cfq_pick_class(cfq);
    if (cls < 0)
        return NULL;

    /* Pick the first queue from this class's list (round-robin) */
    struct list_head *class_list = cfq_class_list(cfq, cls);
    struct list_head *first = class_list->next;
    struct cfq_queue *q = list_entry(first, struct cfq_queue, list);

    /* Remove from class list for service */
    list_del(&q->list);
    INIT_LIST_HEAD(&q->list);

    q->slice_start = now;
    q->dispatched = 0;
    q->last_fetch = timer_get_ns();
    cfq->current_q = q;
    cfq->current_class = cls;

    if (!q->head)
        return NULL;

    struct blk_request *req = q->head;
    q->head = req->next;
    if (!q->head)
        q->tail = NULL;
    req->next = NULL;
    q->count--;
    q->dispatched++;

    /* Track read/write counters for starvation detection */
    if (req->flags & BLK_REQ_READ)
        cfq->read_dispatched++;
    else
        cfq->read_dispatched = 0;

    return req;
}

/* ── cfq_free — clean up CFQ data ──────────────────────────────── */

static void cfq_free(struct iosched_queue *iq)
{
    struct iosched_cfq_data *cfq = &iq->cfq;

    /* Cancel any pending idle timer */
    if (hrtimer_active(&cfq->idle_timer))
        hrtimer_cancel(&cfq->idle_timer);

    /* Clean up all queue lists */
    for (int i = 0; i < cfq->queue_count; i++) {
        struct cfq_queue *q = &cfq->queues[i];
        if (!list_empty(&q->list))
            list_del(&q->list);
    }

    cfq->queue_count = 0;
    cfq->current_q = NULL;
    cfq->idle_q = NULL;
    cfq->idle_expired = 0;
}

/* ── Kyber helper: determine domain from request flags ──────────── */

static inline int kyber_domain_from_req(struct blk_request *req)
{
    if (req->flags & BLK_REQ_READ)
        return KYBER_READ_DOMAIN;
    if (req->flags & BLK_REQ_WRITE)
        return KYBER_WRITE_DOMAIN;
    if (req->flags & BLK_REQ_DISCARD)
        return KYBER_DISCARD_DOMAIN;
    return KYBER_OTHER_DOMAIN;
}

/* ── Kyber helper: tune token budget for a domain ──────────────── */

static void kyber_tune_tokens(struct iosched_kyber_domain *kd, uint64_t now_ns)
{
    /* Only tune at the configured interval */
    if (now_ns < kd->last_token_tune + KYBER_TOKEN_INTERVAL_NS)
        return;
    kd->last_token_tune = now_ns;

    /* If we have latency samples, adjust based on target vs actual */
    if (kd->lat_samples > 0 && kd->lat_avg > 0) {
        if (kd->lat_avg > kd->target_ns) {
            /* Over target — reduce tokens to back-pressure */
            if (kd->tokens > kd->tokens_min)
                kd->tokens -= KYBER_TOKEN_ADJUST_STEP;
        } else if (kd->lat_avg < (kd->target_ns * 8ULL / 10ULL)) {
            /* Under target by more than 20% — increase tokens */
            if (kd->tokens < kd->tokens_max)
                kd->tokens += KYBER_TOKEN_ADJUST_STEP;
        }
    } else {
        /* No latency data yet — if queue is deep, increase tokens */
        if (kd->count > 0 && kd->tokens < kd->tokens_max)
            kd->tokens += KYBER_TOKEN_ADJUST_STEP;
    }

    /* Clamp to [tokens_min, tokens_max] */
    if (kd->tokens < kd->tokens_min)
        kd->tokens = kd->tokens_min;
    if (kd->tokens > kd->tokens_max)
        kd->tokens = kd->tokens_max;
}

/* ════════════════════════════════════════════════════════════════════
 * Kyber I/O scheduler — latency-optimized via token-based admission
 *
 * Kyber divides requests into domains (READ, WRITE, DISCARD, OTHER)
 * and uses a token-bucket admission scheme to control concurrency per
 * domain.  Token budgets are dynamically adjusted based on observed
 * I/O latencies vs. configurable targets:
 *   - READ:   2ms target, 128–512 tokens
 *   - WRITE:  10ms target, 16–128 tokens
 *   - DISCARD: 40ms target, 1–32 tokens
 *   - OTHER:  20ms target, 8–64 tokens
 *
 * When a domain's average latency exceeds its target, tokens are
 * reduced to create back-pressure.  When latency is below the target,
 * tokens are increased to allow more concurrency.
 *
 * Dispatch uses round-robin across domains (starting at next_domain)
 * to ensure fairness even under mixed workloads.
 * ════════════════════════════════════════════════════════════════════ */

/* ── kyber_submit ──────────────────────────────────────────────── */

static int kyber_submit(struct iosched_queue *iq, struct blk_request *req)
{
    struct iosched_kyber_data *kd = &iq->kyber;
    int domain = kyber_domain_from_req(req);
    struct iosched_kyber_domain *d = &kd->domains[domain];

    /* Append to the domain's FIFO queue */
    d->submitted++;
    if (d->tail) {
        d->tail->next = req;
        d->tail = req;
    } else {
        d->head = req;
        d->tail = req;
    }
    req->next = NULL;
    d->count++;
    kd->total_queued++;

    return 0;
}

/* ── kyber_fetch ───────────────────────────────────────────────── */

static struct blk_request *kyber_fetch(struct iosched_queue *iq)
{
    struct iosched_kyber_data *kd = &iq->kyber;
    uint64_t now_ns = timer_get_ns();

    /* Round-robin across domains starting from next_domain */
    for (int i = 0; i < KYBER_MAX_DOMAINS; i++) {
        int d_idx = (kd->next_domain + i) % KYBER_MAX_DOMAINS;
        struct iosched_kyber_domain *d = &kd->domains[d_idx];

        /* Skip empty domains */
        if (!d->head)
            continue;

        /* Token-admission: check if tokens are available */
        if (d->tokens <= 0)
            continue;

        /* Tune token budget if enough time has passed */
        kyber_tune_tokens(d, now_ns);

        /* Re-check after tuning */
        if (d->tokens <= 0)
            continue;

        /* Dispatch the next request from this domain */
        struct blk_request *req = d->head;
        d->head = req->next;
        if (!d->head)
            d->tail = NULL;
        req->next = NULL;
        d->count--;
        kd->total_queued--;

        /* Record dispatch timestamp in expiry field for latency tracking
         * (Kyber does not use expiry — this field is deadline-specific) */
        req->expiry = timer_get_ns();

        /* Consume a token */
        d->tokens--;
        d->fetched++;

        /* Set next domain start for fairness */
        kd->next_domain = (d_idx + 1) % KYBER_MAX_DOMAINS;

        return req;
    }

    /* No domain has both requests and tokens */
    return NULL;
}

/* ── kyber_free ────────────────────────────────────────────────── */

static void kyber_free(struct iosched_queue *iq)
{
    memset(&iq->kyber, 0, sizeof(iq->kyber));
}

/* ── Kyber latency tracking (called from iosched_request_complete) ─ */

static void kyber_record_latency(struct iosched_kyber_data *kd,
                                  struct blk_request *req)
{
    if (!req) return;

    int domain = kyber_domain_from_req(req);
    struct iosched_kyber_domain *d = &kd->domains[domain];

    /* Compute latency using the dispatch timestamp stored in expiry */
    uint64_t now_ns = timer_get_ns();
    uint64_t lat;

    if (req->expiry > 0 && now_ns > req->expiry)
        lat = now_ns - req->expiry;
    else
        lat = 0;

    /* Exponentially weighted moving average (α ≈ 1/8) */
    if (d->lat_samples == 0) {
        d->lat_avg = lat;
    } else {
        d->lat_avg = (d->lat_avg * 7ULL + lat) / 8ULL;
    }
    d->lat_samples++;
}

/* ── iosched_try_merge ────────────────────────────────────────────
 * Try to merge a request with the last request in the queue.
 * Returns 1 if merged (req was consumed), 0 otherwise.
 * Adjacent conditions:
 *   Back merge:  existing.lba + existing.count == req.lba
 *   Front merge: req.lba + req.count == existing.lba
 */
int iosched_try_merge(struct iosched_queue *iq, struct blk_request *req)
{
    if (!iq || !req) return 0;

    /* Back merge with tail */
    if (iq->tail && same_dir(iq->tail, req) &&
        iq->tail->lba + iq->tail->count == req->lba) {
        iq->tail->count += req->count;
        return 1;
    }

    /* Front merge with head */
    if (iq->head && same_dir(iq->head, req) &&
        req->lba + req->count == iq->head->lba) {
        req->count += iq->head->count;
        iq->head->lba = req->lba;
        /* Replace head with merged request — requires care */
        return 0; /* cannot easily replace; caller does it */
    }

    return 0;
}

/* ── Exported symbols ────────────────────────────────────────────── */
EXPORT_SYMBOL(iosched_init);
EXPORT_SYMBOL(iosched_submit_request);
EXPORT_SYMBOL(iosched_fetch_request);
EXPORT_SYMBOL(iosched_set_policy);
EXPORT_SYMBOL(iosched_get_policy);
EXPORT_SYMBOL(iosched_request_complete);
EXPORT_SYMBOL(iosched_get_queue);

/* ── iosched_merge ────────────────────────────────────── */
int iosched_merge(void *queue, void *req)
{
    (void)queue;
    (void)req;
    return 0;
}
/* ── iosched_dispatch ──────────────────────────────────── */
int iosched_dispatch(void *queue)
{
    (void)queue;
    return 0;
}
/* ── iosched_complete ─────────────────────────────────── */
int iosched_complete(void *req)
{
    (void)req;
    return 0;
}

/* ── Module metadata ─────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("Block I/O scheduler (elevator) — NOOP/DEADLINE/CFQ/KYBER with enhanced merging");
MODULE_AUTHOR("Rusik69 OS Kernel");
