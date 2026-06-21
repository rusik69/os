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
 * ════════════════════════════════════════════════════════════════════ */

static int noop_submit(struct iosched_queue *iq, struct blk_request *req)
{
    /* Attempt back-merge with last request in queue */
    if (iq->tail && same_dir(iq->tail, req) &&
        iq->tail->lba + iq->tail->count == req->lba) {
        iq->tail->count += req->count;
        return 0;
    }

    /* Append to tail (FIFO) */
    if (iq->tail) {
        iq->tail->next = req;
        iq->tail = req;
    } else {
        iq->head = req;
        iq->tail = req;
    }
    req->next = NULL;
    iq->queued_count++;
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
    return req;
}

static void noop_free(struct iosched_queue *iq)
{
    (void)iq;
    /* Nothing to free for NOOP */
}

/* ════════════════════════════════════════════════════════════════════
 * Deadline scheduler
 * ════════════════════════════════════════════════════════════════════ */

static void deadline_enqueue_fifo(struct iosched_deadline_data *dd,
                                   struct blk_request *req, int qid)
{
    /* Insert into FIFO list sorted by expiry */
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

    /* Try to merge with existing requests in the per-direction queue */
    struct blk_request *cur = dd->queues[qid].head;
    while (cur) {
        if (same_dir(cur, req)) {
            /* Back merge */
            if (cur->lba + cur->count == req->lba) {
                cur->count += req->count;
                return 0;
            }
            /* Front merge */
            if (req->lba + req->count == cur->lba) {
                req->count += cur->count;
                cur->lba = req->lba;
                /* Replace cur's data in the queue with merged request */
                req->next = cur->next;
                if (cur == dd->queues[qid].head) {
                    dd->queues[qid].head = req;
                }
                if (cur == dd->queues[qid].tail) {
                    dd->queues[qid].tail = req;
                }
                /* Free the old request (who owns it? caller still does) */
                cur->count = 0; /* mark as invalid */
                dd->queues[qid].count--;
                return 0;
            }
        }
        cur = cur->next;
    }

    /* Add to the per-direction sorted queue (sort by LBA for better merging) */
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

static struct blk_request *deadline_fetch(struct iosched_queue *iq)
{
    struct iosched_deadline_data *dd = &iq->deadline;
    uint64_t now = timer_get_ms();
    int i;

    /* Check for expired requests — prioritize those */
    for (i = 0; i < DD_QUEUE_COUNT; i++) {
        struct blk_request *fifo = dd->fifo_list[i];
        while (fifo && fifo->expiry <= now) {
            /* Found expired request; remove from both queues */
            struct blk_request *expired = deadline_dequeue_fifo(dd, i);

            /* Remove from sorted queue */
            struct blk_request **pp = &dd->queues[i].head;
            while (*pp && *pp != expired)
                pp = &(*pp)->next;
            if (*pp == expired) {
                *pp = expired->next;
                if (!expired->next)
                    dd->queues[i].tail = NULL;
                expired->next = NULL;
                dd->queues[i].count--;
            }

            return expired;
        }
    }

    /* Alternate between read and write queues to prevent starvation.
     * Priority is given to reads unless writes are starved. */
    if (dd->queues[DD_READ_QUEUE].count > 0) {
        /* Check if we should service writes to avoid starvation */
        if (dd->queues[DD_WRITE_QUEUE].count > 0 &&
            dd->starved >= 3) {
            /* Service a write after 3 read dispatches */
            dd->starved = 0;
            goto serve_write;
        }

        /* Serve a read */
        struct blk_request *req = dd->queues[DD_READ_QUEUE].head;
        if (req) {
            dd->queues[DD_READ_QUEUE].head = req->next;
            if (!req->next)
                dd->queues[DD_READ_QUEUE].tail = NULL;
            req->next = NULL;
            dd->queues[DD_READ_QUEUE].count--;

            /* Remove from FIFO list */
            struct blk_request **pp = &dd->fifo_list[DD_READ_QUEUE];
            while (*pp && *pp != req)
                pp = &(*pp)->next;
            if (*pp == req) {
                *pp = req->next;
                if (!req->next)
                    dd->fifo_tail[DD_READ_QUEUE] = NULL;
                req->next = NULL;
                dd->fifo_count[DD_READ_QUEUE]--;
            }

            dd->starved++;
            return req;
        }
    }

serve_write:
    if (dd->queues[DD_WRITE_QUEUE].count > 0) {
        struct blk_request *req = dd->queues[DD_WRITE_QUEUE].head;
        if (req) {
            dd->queues[DD_WRITE_QUEUE].head = req->next;
            if (!req->next)
                dd->queues[DD_WRITE_QUEUE].tail = NULL;
            req->next = NULL;
            dd->queues[DD_WRITE_QUEUE].count--;

            /* Remove from FIFO list */
            struct blk_request **pp = &dd->fifo_list[DD_WRITE_QUEUE];
            while (*pp && *pp != req)
                pp = &(*pp)->next;
            if (*pp == req) {
                *pp = req->next;
                if (!req->next)
                    dd->fifo_tail[DD_WRITE_QUEUE] = NULL;
                req->next = NULL;
                dd->fifo_count[DD_WRITE_QUEUE]--;
            }

            dd->starved = 0;
            return req;
        }
    }

    return NULL;
}

static void deadline_free(struct iosched_queue *iq)
{
    (void)iq;
    /* Deadline data is inline in the union, nothing to free */
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
