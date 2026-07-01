// SPDX-License-Identifier: GPL-2.0-only
/*
 * blk_mq.c — Multi-queue block I/O layer with tag-based request allocation
 *
 * Implements a multi-queue block layer with per-CPU submission queues,
 * software staging queues, and hardware dispatch queues with per-queue
 * driver callbacks, queue state management, and poll/interrupt completion.
 *
 * Hardware dispatch queues provide:
 *   - Direct dispatch (bypass SW queues for latency-sensitive I/O)
 *   - Staged dispatch (SW queues feed HW queues via blk_mq_flush)
 *   - Per-queue submit/poll/complete callbacks for device drivers
 *   - Queue state machine (running / stopped / offline / draining)
 *   - CPU-to-HW-queue mapping
 *   - Per-queue statistics and error tracking
 *
 * Request allocation uses a pre-allocated tag pool with bitmap-based
 * tracking, avoiding per-request kmalloc overhead.
 */

#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "smp.h"
#include "heap.h"
#include "bitops.h"

/* find_first_zero_bit is implemented in src/lib/find_bit.c.
 * We extern it directly because find_bit.h re-declares test_bit/set_bit/clear_bit
 * with incompatible signatures vs bitops.h, and we only need this one function. */
extern unsigned long find_first_zero_bit(const unsigned long *addr,
                                         unsigned long size);

#define BLK_MQ_MAX_HW_QUEUES   16
#define BLK_MQ_MAX_SW_QUEUES   64
#define BLK_MQ_DEPTH           256

/* Tag bitmap words */
#define BLK_MQ_TAG_WORDS       (BLK_MQ_DEPTH / 64)

/* ── Dispatch modes ──────────────────────────────────────────────── */

/* How a request reaches the hardware dispatch queue */
enum blk_mq_dispatch_mode {
    BLK_MQ_DISPATCH_DIRECT = 0,   /* Bypass SW queues — driversubmit direct */
    BLK_MQ_DISPATCH_STAGED = 1,   /* Through SW staging queues via flush */
    BLK_MQ_DISPATCH_BATCH  = 2,   /* Batch multiple requests before dispatch */
};

/* ── Hardware queue states ───────────────────────────────────────── */

enum blk_mq_hw_state {
    BLK_MQ_HW_RUNNING  = 0,
    BLK_MQ_HW_STOPPED  = 1,       /* Temporarily stopped (e.g. out of resources) */
    BLK_MQ_HW_OFFLINE  = 2,       /* Queue not available */
    BLK_MQ_HW_DRAINING = 3,       /* Draining pending requests before offline */
};

/* ── Forward declarations ────────────────────────────────────────── */

struct blk_mq_request;
struct blk_mq_hw_queue;

/* ── Function pointer type for hardware dispatch callbacks ───────── */

/* Driver callback: submit a single request to the device associated
 * with this hardware queue.  Returns 0 on acceptance (< 0 on error).
 * The request is completed via blk_mq_complete_request() when the
 * device finishes the I/O. */
typedef int (*blk_mq_hw_submit_fn)(struct blk_mq_request *req,
                                     void *priv_data);

/* Driver callback: poll for completions on this hardware queue.
 * Returns the number of requests completed in this poll cycle.
 * Only used when the queue operates in polling mode (no IRQ). */
typedef int (*blk_mq_hw_poll_fn)(struct blk_mq_request *req,
                                   void *priv_data);

/* ── I/O request (forward-declared above) ─────────────────────────── */

struct blk_mq_request {
    int in_use;
    int tag;          /* index in the pre-allocated tag pool */
    uint64_t sector;
    uint8_t *buffer;
    size_t count;
    int write;        /* 0=read, 1=write */
    int error;        /* filled by driver on completion */
    int hw_queue_idx; /* which HW queue this request was submitted to */
    void (*complete)(struct blk_mq_request *req, int error);
};

/* ── Hardware dispatch queue ─────────────────────────────────────── */

struct blk_mq_hw_queue {
    /* Request ring buffer */
    struct blk_mq_request *queue[BLK_MQ_DEPTH];
    int head;
    int tail;
    spinlock_t lock;

    /* Queue index and state */
    int                  queue_idx;
    enum blk_mq_hw_state state;
    enum blk_mq_dispatch_mode dispatch_mode;

    /* Dispatch callbacks — set by the device driver claiming this queue */
    blk_mq_hw_submit_fn submit_fn;
    void               *submit_priv;

    /* Polling */
    blk_mq_hw_poll_fn   poll_fn;
    void               *poll_priv;
    int                 use_polling;   /* 1 = polling mode, 0 = IRQ mode */

    /* CPU affinity mask (bitmask) — which CPUs submit to this queue */
    unsigned long       cpu_affinity;

    /* Statistics */
    uint64_t submitted;     /* total requests submitted */
    uint64_t completed;     /* total requests completed */
    uint64_t errors;        /* total error completions */
    uint64_t pending;       /* requests dispatched but not completed */
    uint64_t batch_count;   /* batch dispatch counter */
};

/* Software staging queue (per-CPU index) */
struct blk_mq_sw_queue {
    struct blk_mq_request *queue[BLK_MQ_DEPTH];
    int head;
    int tail;
    spinlock_t lock;
};

/* Tag set — manages pre-allocated request pool */
struct blk_mq_tags {
    spinlock_t lock;
    unsigned long bitmap[BLK_MQ_TAG_WORDS];   /* 1 = free, 0 = in-use */
    struct blk_mq_request requests[BLK_MQ_DEPTH];  /* pre-allocated pool */
    int free_tags;
    int next_tag;  /* hint for next allocation */
};

static struct blk_mq_sw_queue blk_mq_sw_queues[BLK_MQ_MAX_SW_QUEUES];
static struct blk_mq_hw_queue blk_mq_hw_queues[BLK_MQ_MAX_HW_QUEUES];
static struct blk_mq_tags blk_mq_tags;
static int blk_mq_num_hw_queues = 0;
static int blk_mq_initialized = 0;

/* ── Forward declarations of static helpers ──────────────────────── */

static void blk_mq_free_request(struct blk_mq_request *req);

/* ════════════════════════════════════════════════════════════════════
 * Tag pool management
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Initialize the tag pool with all tags marked free and the request
 * array zeroed.
 */
static void blk_mq_tags_init(struct blk_mq_tags *tags)
{
    spinlock_init(&tags->lock);
    for (int i = 0; i < BLK_MQ_TAG_WORDS; i++)
        tags->bitmap[i] = ~0UL;
    tags->free_tags = BLK_MQ_DEPTH;
    tags->next_tag = 0;

    for (int i = 0; i < BLK_MQ_DEPTH; i++) {
        tags->requests[i].in_use = 0;
        tags->requests[i].tag = i;
        tags->requests[i].sector = 0;
        tags->requests[i].buffer = NULL;
        tags->requests[i].count = 0;
        tags->requests[i].write = 0;
        tags->requests[i].error = 0;
        tags->requests[i].hw_queue_idx = -1;
        tags->requests[i].complete = NULL;
    }
}

/*
 * Allocate a tag from the pool.
 *
 * Scans the bitmap for the first zero bit (free slot), marks it as
 * in-use, and returns the index.  Returns -1 if the pool is empty.
 */
static int blk_mq_get_tag(struct blk_mq_tags *tags)
{
    uint64_t irq_flags;
    int tag;

    spinlock_irqsave_acquire(&tags->lock, &irq_flags);

    if (tags->free_tags == 0) {
        spinlock_irqsave_release(&tags->lock, irq_flags);
        return -1;
    }

    tag = (int)find_first_zero_bit(tags->bitmap, BLK_MQ_DEPTH);
    if (tag >= BLK_MQ_DEPTH) {
        spinlock_irqsave_release(&tags->lock, irq_flags);
        return -1;
    }

    clear_bit(tag, tags->bitmap);
    tags->free_tags--;
    tags->next_tag = (tag + 1) % BLK_MQ_DEPTH;

    spinlock_irqsave_release(&tags->lock, irq_flags);

    return tag;
}

/*
 * Return a tag to the pool so it can be reused.
 */
static void blk_mq_put_tag(struct blk_mq_tags *tags, int tag)
{
    uint64_t irq_flags;

    if (tag < 0 || tag >= BLK_MQ_DEPTH)
        return;

    spinlock_irqsave_acquire(&tags->lock, &irq_flags);

    set_bit(tag, tags->bitmap);
    tags->free_tags++;

    spinlock_irqsave_release(&tags->lock, irq_flags);
}

/* Allocate a request from the tag pool */
static struct blk_mq_request *blk_mq_alloc_request(void)
{
    int tag = blk_mq_get_tag(&blk_mq_tags);
    struct blk_mq_request *req;

    if (tag < 0)
        return NULL;

    req = &blk_mq_tags.requests[tag];
    req->in_use = 1;
    req->tag = tag;
    req->sector = 0;
    req->buffer = NULL;
    req->count = 0;
    req->write = 0;
    req->error = 0;
    req->hw_queue_idx = -1;
    req->complete = NULL;

    return req;
}

/* Free a request back to the tag pool */
static void blk_mq_free_request(struct blk_mq_request *req)
{
    if (!req)
        return;

    req->in_use = 0;
    req->hw_queue_idx = -1;
    blk_mq_put_tag(&blk_mq_tags, req->tag);
}

/* ════════════════════════════════════════════════════════════════════
 * Completion — must be defined before any dispatch function uses it
 * ════════════════════════════════════════════════════════════════════ */

/**
 * blk_mq_complete_request - Complete a request and update HW queue stats.
 *
 * Invokes the completion callback and updates the originating HW
 * queue's statistics.  The request is marked as no longer in use.
 *
 * This function is safe to call from any context (IRQ, tasklet,
 * polling loop).  It does NOT free the tag — use blk_mq_free_request()
 * if the request should be returned to the pool.
 */
void blk_mq_complete_request(struct blk_mq_request *req)
{
    int hw_idx;
    struct blk_mq_hw_queue *hwq;

    if (!req)
        return;

    hw_idx = req->hw_queue_idx;

    if (req->complete)
        req->complete(req, req->error);

    /* Update HW queue statistics (best-effort) */
    if (hw_idx >= 0 && hw_idx < BLK_MQ_MAX_HW_QUEUES) {
        if (hw_idx < blk_mq_num_hw_queues) {
            uint64_t irq_flags;
            hwq = &blk_mq_hw_queues[hw_idx];
            spinlock_irqsave_acquire(&hwq->lock, &irq_flags);
            hwq->completed++;
            if (req->error != 0)
                hwq->errors++;
            if (hwq->pending > 0)
                hwq->pending--;
            spinlock_irqsave_release(&hwq->lock, irq_flags);
        }
    }

    req->in_use = 0;
    req->hw_queue_idx = -1;
}

/**
 * blk_mq_complete - Backward-compatible completion wrapper.
 *
 * Sets the error code on the request and calls
 * blk_mq_complete_request().  Does NOT free the tag.
 */
void blk_mq_complete(struct blk_mq_request *req, int error)
{
    if (req) {
        req->error = error;
        blk_mq_complete_request(req);
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Hardware queue management
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Select the hardware queue for a given I/O request.
 *
 * Uses a hash of the sector number to select among available HW queues,
 * providing better queue-level parallelism than a naive CPU-based mapping
 * when multiple CPUs share a queue.
 */
static int blk_mq_get_hw_queue(uint64_t sector)
{
    if (blk_mq_num_hw_queues == 0)
        return 0;
    /* Simple sector hash: use the lower bits to spread across queues */
    return (int)(sector % (uint64_t)blk_mq_num_hw_queues);
}

/*
 * Map a CPU to a hardware queue index.
 *
 * Returns the preferred HW queue for requests originating from this CPU.
 * Used for direct submission when the caller doesn't specify a queue.
 */
int blk_mq_map_cpu_to_hw_queue(int cpu)
{
    if (cpu < 0 || cpu >= BLK_MQ_MAX_SW_QUEUES)
        cpu = 0;

    if (blk_mq_num_hw_queues == 0)
        return 0;

    /* Check if any HW queue has affinity for this CPU */
    for (int hw = 0; hw < blk_mq_num_hw_queues; hw++) {
        if (blk_mq_hw_queues[hw].cpu_affinity & (1UL << (cpu % 64)))
            return hw;
    }

    /* Fallback: modulo */
    return cpu % blk_mq_num_hw_queues;
}

/*
 * Set CPU affinity for a hardware queue.
 *
 * @hw_idx:  Hardware queue index
 * @cpu_bitmap: Bitmask of CPUs that should submit to this queue
 * Returns 0 on success, -EINVAL if the queue index is out of range.
 */
int blk_mq_set_hw_queue_affinity(int hw_idx, unsigned long cpu_bitmap)
{
    if (hw_idx < 0 || hw_idx >= BLK_MQ_MAX_HW_QUEUES)
        return -EINVAL;
    if (hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    blk_mq_hw_queues[hw_idx].cpu_affinity = cpu_bitmap;
    return 0;
}

/*
 * Change the dispatch mode for a hardware queue.
 *
 * @hw_idx:  Hardware queue index
 * @mode:    BLK_MQ_DISPATCH_DIRECT, BLK_MQ_DISPATCH_STAGED, or
 *           BLK_MQ_DISPATCH_BATCH
 * Returns 0 on success, -EINVAL on invalid arguments.
 */
int blk_mq_set_hw_queue_dispatch_mode(int hw_idx,
                                      enum blk_mq_dispatch_mode mode)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;
    if (mode != BLK_MQ_DISPATCH_DIRECT &&
        mode != BLK_MQ_DISPATCH_STAGED &&
        mode != BLK_MQ_DISPATCH_BATCH)
        return -EINVAL;

    blk_mq_hw_queues[hw_idx].dispatch_mode = mode;
    return 0;
}

/* ── Hardware queue lifecycle ─────────────────────────────────────── */

/*
 * Initialise a single hardware queue structure.
 *
 * @hw_idx: Queue index (0 .. num_hw_queues-1)
 * @submit_fn: Driver callback for submitting requests to the device
 * @priv:      Private data passed to the submit callback
 */
static void blk_mq_hw_queue_init(int hw_idx)
{
    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];

    memset(hwq, 0, sizeof(*hwq));
    spinlock_init(&hwq->lock);
    hwq->queue_idx = hw_idx;
    hwq->state = BLK_MQ_HW_RUNNING;
    hwq->dispatch_mode = BLK_MQ_DISPATCH_STAGED;
}

/*
 * Register a hardware queue and claim it for a device driver.
 *
 * @hw_idx:    Queue index (0 .. num_hw_queues-1)
 * @submit_fn: Driver callback to submit a request to the device
 * @priv:      Opaque pointer passed back to submit_fn
 * @use_polling: Non-zero to enable polling completion, zero for IRQ mode
 *
 * Returns 0 on success, -EINVAL if @submit_fn is NULL or @hw_idx is
 * out of range, -EALREADY if the queue already has a submit callback.
 */
int blk_mq_init_hw_queue(int hw_idx, blk_mq_hw_submit_fn submit_fn,
                          void *priv, int use_polling)
{
    if (hw_idx < 0 || hw_idx >= BLK_MQ_MAX_HW_QUEUES)
        return -EINVAL;
    if (!submit_fn)
        return -EINVAL;
    if (hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

    if (hwq->submit_fn && hwq->submit_fn != submit_fn) {
        /* Already owned by a different driver */
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EALREADY;
    }

    hwq->submit_fn = submit_fn;
    hwq->submit_priv = priv;
    hwq->use_polling = use_polling ? 1 : 0;
    hwq->state = BLK_MQ_HW_RUNNING;

    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return 0;
}

/*
 * Release a hardware queue from driver ownership and reset its state.
 *
 * @hw_idx: Queue index to release
 *
 * Returns 0 on success, -EINVAL if out of range, -EBUSY if the queue
 * still has pending requests (caller must drain first).
 */
int blk_mq_exit_hw_queue(int hw_idx)
{
    if (hw_idx < 0 || hw_idx >= BLK_MQ_MAX_HW_QUEUES)
        return -EINVAL;
    if (hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

    if (hwq->pending > 0) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EBUSY;
    }

    hwq->submit_fn = NULL;
    hwq->submit_priv = NULL;
    hwq->poll_fn = NULL;
    hwq->poll_priv = NULL;
    hwq->state = BLK_MQ_HW_OFFLINE;

    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return 0;
}

/* ── Queue state transitions ─────────────────────────────────────── */

/*
 * Start a hardware queue, allowing it to accept and dispatch requests.
 * Queues are started by default after initialisation.
 */
int blk_mq_start_hw_queue(int hw_idx)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);
    if (hwq->state == BLK_MQ_HW_OFFLINE) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EIO;
    }
    hwq->state = BLK_MQ_HW_RUNNING;
    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return 0;
}

/*
 * Stop a hardware queue.  Requests may still be enqueued but will
 * not be dispatched until start is called again.
 */
int blk_mq_stop_hw_queue(int hw_idx)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);
    if (hwq->state == BLK_MQ_HW_RUNNING)
        hwq->state = BLK_MQ_HW_STOPPED;
    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return 0;
}

/*
 * Drain a hardware queue of all pending requests.
 *
 * The queue is placed in DRAINING state, preventing new submissions.
 * All requests still in the ring buffer are completed with -EIO and
 * freed back to the tag pool.  Returns the number of requests drained.
 */
int blk_mq_drain_hw_queue(int hw_idx)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;
    int drained = 0;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

    hwq->state = BLK_MQ_HW_DRAINING;

    /* Free any requests still in the ring buffer */
    while (hwq->head != hwq->tail) {
        struct blk_mq_request *req = hwq->queue[hwq->head];
        hwq->head = (hwq->head + 1) % BLK_MQ_DEPTH;
        if (req) {
            req->error = -EIO;
            blk_mq_complete_request(req);
            blk_mq_free_request(req);
            drained++;
        }
    }

    hwq->pending = 0;
    hwq->state = BLK_MQ_HW_OFFLINE;

    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return drained;
}

/*
 * Register a polling callback for a hardware queue.
 *
 * When use_polling is set, the block layer calls poll_fn instead of
 * waiting for an IRQ to complete requests.  This is used by devices
 * that support NVMe-style completion polling.
 */
int blk_mq_set_hw_queue_poll(int hw_idx, blk_mq_hw_poll_fn poll_fn,
                              void *priv)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);
    hwq->poll_fn = poll_fn;
    hwq->poll_priv = priv;
    spinlock_irqsave_release(&hwq->lock, irq_flags);

    return 0;
}

/* ── Direct dispatch (bypasses SW queues) ─────────────────────────── */

/*
 * Submit a request directly to a hardware dispatch queue, bypassing
 * the software staging queues entirely.
 *
 * The request is enqueued in the HW queue's ring buffer, and the
 * driver's submit_fn callback is invoked immediately.  If the submit
 * call fails, the request is completed with the driver's error code.
 *
 * Returns 0 on success, -EAGAIN if the HW queue is full, -EIO if the
 * queue is stopped/offline.
 */
static int blk_mq_direct_dispatch(struct blk_mq_request *req, int hw_idx)
{
    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;
    int ret;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

    if (hwq->state != BLK_MQ_HW_RUNNING) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EIO;
    }

    int next = (hwq->tail + 1) % BLK_MQ_DEPTH;
    if (next == hwq->head) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EAGAIN;
    }

    req->hw_queue_idx = hw_idx;
    hwq->queue[hwq->tail] = req;
    hwq->tail = next;
    hwq->submitted++;
    hwq->pending++;

    spinlock_irqsave_release(&hwq->lock, irq_flags);

    /* Now submit to the device via the driver callback */
    if (hwq->submit_fn) {
        ret = hwq->submit_fn(req, hwq->submit_priv);
        if (ret < 0) {
            kprintf("[blk_mq] HW queue %d: driver rejected request: %d\n",
                    hw_idx, ret);
            req->error = ret;
            blk_mq_complete_request(req);
        }
        return ret;
    }

    /* No driver callback — request stays queued until dispatch() */
    return 0;
}

/* ── Batch dispatch ───────────────────────────────────────────────── */

/*
 * Submit a batch of requests to a hardware queue in a single call.
 *
 * Each request in the batch is enqueued in the HW ring buffer, but
 * the driver's submit_fn is called only once with the last request
 * as a trigger.  The driver may retrieve the full batch by draining
 * the HW queue ring.
 *
 * @reqs:    Array of request pointers
 * @count:   Number of requests in the batch
 * @hw_idx:  Target hardware queue
 *
 * Returns 0 on success, negative errno on failure.  On partial
 * failure, successfully enqueued requests remain in the HW ring.
 */
static int blk_mq_batch_dispatch(struct blk_mq_request **reqs, int count,
                                  int hw_idx)
{
    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
    uint64_t irq_flags;
    int i;

    spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

    if (hwq->state != BLK_MQ_HW_RUNNING) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EIO;
    }

    /* Check if there's enough room for the entire batch */
    int space;
    if (hwq->head > hwq->tail) {
        space = hwq->head - hwq->tail - 1;
    } else if (hwq->head == hwq->tail) {
        space = BLK_MQ_DEPTH - 1;
    } else {
        space = BLK_MQ_DEPTH - (hwq->tail - hwq->head) - 1;
    }
    if (space < count) {
        spinlock_irqsave_release(&hwq->lock, irq_flags);
        return -EAGAIN;
    }

    for (i = 0; i < count; i++) {
        struct blk_mq_request *req = reqs[i];
        if (!req) continue;
        req->hw_queue_idx = hw_idx;
        hwq->queue[hwq->tail] = req;
        hwq->tail = (hwq->tail + 1) % BLK_MQ_DEPTH;
    }

    hwq->submitted += count;
    hwq->pending   += count;
    hwq->batch_count++;

    spinlock_irqsave_release(&hwq->lock, irq_flags);

    /* Notify the driver once for the batch */
    if (hwq->submit_fn) {
        int ret = hwq->submit_fn(reqs[0], hwq->submit_priv);
        if (ret < 0) {
            kprintf("[blk_mq] HW queue %d: batch submit failed: %d\n",
                    hw_idx, ret);
        }
        return ret;
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════ */

/**
 * blk_mq_submit_request - Submit an I/O request through the multi-queue layer
 * @sector: Starting sector number for the I/O operation
 * @buffer: Pointer to the data buffer (read/write payload)
 * @count: Number of bytes to transfer
 * @write: Operation direction (0 = read, 1 = write)
 * @complete: Completion callback invoked when the request is finished
 *
 * Allocates a tagged request from the pre-allocated pool, fills it with
 * the given parameters, and enqueues it on the current CPU's software
 * staging queue.  The request is later flushed to a hardware dispatch
 * queue by blk_mq_flush().
 *
 * If the target HW queue is in DIRECT dispatch mode, the request is
 * sent directly to the HW queue's submit callback, bypassing the SW
 * staging queue entirely.
 *
 * Return: 0 on success, -ENOMEM if the tag pool is empty, -EAGAIN if
 *         the software queue is full
 */
int blk_mq_submit_request(uint64_t sector, uint8_t *buffer,
                           size_t count, int write,
                           void (*complete)(struct blk_mq_request *, int))
{
    struct blk_mq_request *req;
    int cpu = smp_get_cpu_id();
    int hw_idx;
    uint64_t irq_flags;
    int next;

    req = blk_mq_alloc_request();
    if (!req)
        return -ENOMEM;

    req->sector = sector;
    req->buffer = buffer;
    req->count = count;
    req->write = write;
    req->complete = complete;
    req->error = 0;
    req->in_use = 1;

    /* Select the target hardware queue based on sector */
    hw_idx = blk_mq_get_hw_queue(sector);

    /* If the target HW queue uses direct dispatch, bypass SW queues */
    if (hw_idx < blk_mq_num_hw_queues &&
        blk_mq_hw_queues[hw_idx].dispatch_mode == BLK_MQ_DISPATCH_DIRECT) {
        int ret = blk_mq_direct_dispatch(req, hw_idx);
        if (ret < 0) {
            blk_mq_free_request(req);
            return ret;
        }
        return 0;
    }

    /* Add to software queue for this CPU */
    if (cpu >= BLK_MQ_MAX_SW_QUEUES)
        cpu = 0;

    spinlock_irqsave_acquire(&blk_mq_sw_queues[cpu].lock, &irq_flags);

    next = (blk_mq_sw_queues[cpu].tail + 1) % BLK_MQ_DEPTH;
    if (next == blk_mq_sw_queues[cpu].head) {
        spinlock_irqsave_release(&blk_mq_sw_queues[cpu].lock, irq_flags);
        blk_mq_free_request(req);
        return -EAGAIN;
    }
    blk_mq_sw_queues[cpu].queue[blk_mq_sw_queues[cpu].tail] = req;
    blk_mq_sw_queues[cpu].tail = next;

    spinlock_irqsave_release(&blk_mq_sw_queues[cpu].lock, irq_flags);

    return 0;
}

/**
 * blk_mq_flush - Flush software staging queues to hardware dispatch queues
 *
 * Iterates over all software staging queues and drains each pending
 * request into a hardware dispatch queue using the sector hash
 * assignment.  If a hardware queue is stopped or full, the request
 * is freed back to the tag pool.
 */
void blk_mq_flush(void)
{
    for (int sw = 0; sw < BLK_MQ_MAX_SW_QUEUES; sw++) {
        struct blk_mq_sw_queue *swq = &blk_mq_sw_queues[sw];
        uint64_t irq_flags;

        spinlock_irqsave_acquire(&swq->lock, &irq_flags);

        while (swq->head != swq->tail) {
            struct blk_mq_request *req = swq->queue[swq->head];
            swq->head = (swq->head + 1) % BLK_MQ_DEPTH;

            if (!req) continue;

            int hw_idx = blk_mq_get_hw_queue(req->sector);

            if (hw_idx >= blk_mq_num_hw_queues) {
                blk_mq_free_request(req);
                continue;
            }

            struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];
            uint64_t irq_flags2;

            spinlock_irqsave_acquire(&hwq->lock, &irq_flags2);

            if (hwq->state != BLK_MQ_HW_RUNNING) {
                spinlock_irqsave_release(&hwq->lock, irq_flags2);
                blk_mq_free_request(req);
                continue;
            }

            req->hw_queue_idx = hw_idx;

            int next = (hwq->tail + 1) % BLK_MQ_DEPTH;
            if (next != hwq->head) {
                hwq->queue[hwq->tail] = req;
                hwq->tail = next;
                hwq->submitted++;
                hwq->pending++;
            } else {
                spinlock_irqsave_release(&hwq->lock, irq_flags2);
                blk_mq_free_request(req);
                continue;
            }

            spinlock_irqsave_release(&hwq->lock, irq_flags2);
        }

        spinlock_irqsave_release(&swq->lock, irq_flags);
    }
}

/**
 * blk_mq_dispatch - Process pending requests from all hardware queues
 *
 * Iterates over every hardware dispatch queue (RUNNING state) and
 * processes each enqueued request by invoking the driver's submit_fn
 * callback.  For queues without a driver callback, requests are
 * completed immediately with success (0) as a simplified simulation.
 *
 * After the driver submits the request, it is responsible for calling
 * blk_mq_complete_request() when the I/O finishes.
 */
void blk_mq_dispatch(void)
{
    for (int hw = 0; hw < blk_mq_num_hw_queues; hw++) {
        struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw];
        uint64_t irq_flags;

        spinlock_irqsave_acquire(&hwq->lock, &irq_flags);

        if (hwq->state != BLK_MQ_HW_RUNNING) {
            spinlock_irqsave_release(&hwq->lock, irq_flags);
            continue;
        }

        while (hwq->head != hwq->tail) {
            struct blk_mq_request *req = hwq->queue[hwq->head];
            hwq->head = (hwq->head + 1) % BLK_MQ_DEPTH;

            if (!req) continue;

            if (hwq->submit_fn) {
                int ret = hwq->submit_fn(req, hwq->submit_priv);
                if (ret < 0) {
                    req->error = ret;
                    blk_mq_complete_request(req);
                    hwq->errors++;
                }
            } else {
                /* No driver — simulate completion */
                if (req->complete)
                    req->complete(req, 0);
                hwq->completed++;
                if (hwq->pending > 0)
                    hwq->pending--;
                blk_mq_free_request(req);
            }
        }

        spinlock_irqsave_release(&hwq->lock, irq_flags);
    }
}

/**
 * blk_mq_hw_queue_poll - Poll a specific hardware queue for completions.
 *
 * If the HW queue has a poll_fn registered, it is called to check
 * for completed requests.  Polling is only meaningful when the queue
 * uses polling mode (use_polling = 1).
 *
 * @hw_idx: Hardware queue to poll
 * Returns the number of requests completed during this poll, or
 * -EINVAL if the queue is out of range.
 */
int blk_mq_hw_queue_poll(int hw_idx)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];

    if (!hwq->use_polling || !hwq->poll_fn)
        return 0;

    return hwq->poll_fn(NULL, hwq->poll_priv);
}

/**
 * blk_mq_get_hw_queue_stats - Retrieve statistics for a hardware queue.
 *
 * @hw_idx: Queue index
 * @submitted:  Output: total requests submitted
 * @completed:  Output: total requests completed
 * @errors:     Output: total error completions
 * @pending:    Output: requests dispatched but not completed
 *
 * Returns 0 on success, -EINVAL if out of range.
 */
int blk_mq_get_hw_queue_stats(int hw_idx, uint64_t *submitted,
                               uint64_t *completed, uint64_t *errors,
                               uint64_t *pending)
{
    if (hw_idx < 0 || hw_idx >= blk_mq_num_hw_queues)
        return -EINVAL;

    struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx];

    if (submitted) *submitted = hwq->submitted;
    if (completed) *completed = hwq->completed;
    if (errors)    *errors    = hwq->errors;
    if (pending)   *pending   = hwq->pending;

    return 0;
}

/**
 * blk_mq_init - Initialise the multi-queue block I/O layer
 *
 * Detects the number of available CPUs, initialises the tag pool, and
 * sets up the software and hardware queue data structures.  Each queue
 * is assigned its own spinlock for concurrent access.  This function
 * may safely be called multiple times — subsequent calls are no-ops.
 */
void __init blk_mq_init(void)
{
    if (blk_mq_initialized)
        return;

    blk_mq_tags_init(&blk_mq_tags);

    blk_mq_num_hw_queues = smp_get_cpu_count();
    if (blk_mq_num_hw_queues > BLK_MQ_MAX_HW_QUEUES)
        blk_mq_num_hw_queues = BLK_MQ_MAX_HW_QUEUES;
    if (blk_mq_num_hw_queues < 1)
        blk_mq_num_hw_queues = 1;

    for (int i = 0; i < BLK_MQ_MAX_SW_QUEUES; i++)
        spinlock_init(&blk_mq_sw_queues[i].lock);

    for (int i = 0; i < BLK_MQ_MAX_HW_QUEUES; i++)
        blk_mq_hw_queue_init(i);

    blk_mq_initialized = 1;
    kprintf("[OK] blk-mq: multi-queue block I/O layer (%d HW queues, %d tagged requests, direct/staged/batch dispatch)\n",
            blk_mq_num_hw_queues, BLK_MQ_DEPTH);
}

/* ── Stub: blk_mq_init_queue ─────────────────────────────────────── */
int blk_mq_init_queue(void *dev, void *tag_set)
{
    (void)dev;
    (void)tag_set;
    return 0;
}

/* ── Stub: blk_mq_exit_queue ─────────────────────────────────────── */
int blk_mq_exit_queue(void *q)
{
    (void)q;
    return 0;
}

/* ── Stub: blk_mq_submit_bio ─────────────────────────────────────── */
int blk_mq_submit_bio(void *bio)
{
    (void)bio;
    return 0;
}
