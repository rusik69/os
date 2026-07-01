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
    kprintf("[OK] iosched: I/O scheduler initialized (NOOP/DEADLINE/CFQ)\n");
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
    if (policy != IOSCHED_NOOP && policy != IOSCHED_DEADLINE && policy != IOSCHED_CFQ)
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
        INIT_LIST_HEAD(&iq->cfq.active_queues);
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

void iosched_request_complete(int dev_id, struct blk_request *req)
{
    (void)dev_id;
    (void)req;
    /* CFQ uses completion to re-arm slices; for now this is a no-op
     * as the existing block layer handles completion. */
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
 * ════════════════════════════════════════════════════════════════════ */

/* Find or create a per-process queue */
static struct cfq_queue *cfq_get_queue(struct iosched_cfq_data *cfq,
                                        uint64_t pid)
{
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
    cfq->queue_count++;

    return q;
}

static int cfq_submit(struct iosched_queue *iq, struct blk_request *req)
{
    struct iosched_cfq_data *cfq = &iq->cfq;
    uint64_t pid;

    /* Get the PID of the submitting process */
    struct process *proc = process_get_current();
    pid = proc ? (uint64_t)(proc->pid) : 0;

    struct cfq_queue *q = cfq_get_queue(cfq, pid);
    if (!q) {
        /* Fall back to queue 0 if max queues reached */
        q = &cfq->queues[0];
    }

    /* Try to merge with last request in this per-process queue */
    if (q->tail && same_dir(q->tail, req) &&
        q->tail->lba + q->tail->count == req->lba) {
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

    /* Add to active list if first request */
    if (q->count == 1 && list_empty(&q->list))
        list_add_tail(&q->list, &cfq->active_queues);

    return 0;
}

static struct blk_request *cfq_fetch(struct iosched_queue *iq)
{
    struct iosched_cfq_data *cfq = &iq->cfq;
    uint64_t now = timer_get_ms();

    /* If we have a current queue, check if its slice expired */
    if (cfq->current_q) {
        uint64_t elapsed = now - cfq->current_q->slice_start;
        /* Allow a bit of slack (10 ms) before switching */
        if (elapsed >= CFQ_SLICE_MS + 10 || cfq->current_q->count == 0) {
            /* Move current queue to end of round-robin */
            if (cfq->current_q->count > 0) {
                list_del(&cfq->current_q->list);
                list_add_tail(&cfq->current_q->list, &cfq->active_queues);
            } else {
                list_del(&cfq->current_q->list);
                INIT_LIST_HEAD(&cfq->current_q->list);
            }
            cfq->current_q = NULL;
        }
    }

    /* Pick the next queue in round-robin */
    if (!cfq->current_q) {
        if (list_empty(&cfq->active_queues))
            return NULL;

        struct list_head *first = cfq->active_queues.next;
        struct cfq_queue *q = list_entry(first, struct cfq_queue, list);

        /* Remove from active list (will re-add if still has requests later) */
        list_del(&q->list);
        INIT_LIST_HEAD(&q->list);

        q->slice_start = now;
        q->dispatched = 0;
        cfq->current_q = q;
    }

    /* Dispatch from the current queue */
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

    /* If queue is now empty, we'll pick a new one next time */
    if (q->count == 0) {
        cfq->current_q = NULL;  /* will pick next on next fetch */
    }

    return req;
}

static void cfq_free(struct iosched_queue *iq)
{
    struct iosched_cfq_data *cfq = &iq->cfq;
    for (int i = 0; i < cfq->queue_count; i++) {
        struct cfq_queue *q = &cfq->queues[i];
        if (!list_empty(&q->list))
            list_del(&q->list);
        /* Note: requests themselves are freed by the caller */
    }
    cfq->queue_count = 0;
    cfq->current_q = NULL;
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
MODULE_DESCRIPTION("Block I/O scheduler (elevator) — NOOP/DEADLINE/CFQ with enhanced merging");
MODULE_AUTHOR("Rusik69 OS Kernel");
