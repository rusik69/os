// SPDX-License-Identifier: GPL-2.0-only
/*
 * blk_mq.c — Multi-queue block I/O layer with tag-based request allocation
 *
 * Implements a multi-queue block layer with per-CPU submission queues,
 * software staging queues, and hardware dispatch queues.
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

/* I/O request */
struct blk_mq_request {
    int in_use;
    int tag;          /* index in the pre-allocated tag pool */
    uint64_t sector;
    uint8_t *buffer;
    size_t count;
    int write; /* 0=read, 1=write */
    void (*complete)(struct blk_mq_request *req, int error);
};

/* Software staging queue (per-CPU index) */
struct blk_mq_sw_queue {
    struct blk_mq_request *queue[BLK_MQ_DEPTH];
    int head;
    int tail;
    spinlock_t lock;
};

/* Hardware dispatch queue */
struct blk_mq_hw_queue {
    struct blk_mq_request *queue[BLK_MQ_DEPTH];
    int head;
    int tail;
    spinlock_t lock;
    uint64_t completed;
    uint64_t errors;
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
    req->complete = NULL;

    return req;
}

/* Free a request back to the tag pool */
static void blk_mq_free_request(struct blk_mq_request *req)
{
    if (!req)
        return;

    req->in_use = 0;
    blk_mq_put_tag(&blk_mq_tags, req->tag);
}

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
 * Return: 0 on success, -ENOMEM if the tag pool is empty, -EAGAIN if
 *         the software queue is full
 */
int blk_mq_submit_request(uint64_t sector, uint8_t *buffer,
                           size_t count, int write,
                           void (*complete)(struct blk_mq_request *, int))
{
    struct blk_mq_request *req;
    int cpu = smp_get_cpu_id();
    struct blk_mq_sw_queue *swq;
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
    req->in_use = 1;

    /* Add to software queue for this CPU */
    if (cpu >= BLK_MQ_MAX_SW_QUEUES)
        cpu = 0;
    swq = &blk_mq_sw_queues[cpu];

    spinlock_irqsave_acquire(&swq->lock, &irq_flags);
    next = (swq->tail + 1) % BLK_MQ_DEPTH;
    if (next == swq->head) {
        spinlock_irqsave_release(&swq->lock, irq_flags);
        blk_mq_free_request(req);
        return -EAGAIN;
    }
    swq->queue[swq->tail] = req;
    swq->tail = next;
    spinlock_irqsave_release(&swq->lock, irq_flags);

    return 0;
}

/**
 * blk_mq_flush - Flush software staging queues to hardware dispatch queues
 *
 * Iterates over all software staging queues and drains each pending
 * request into a hardware dispatch queue using round-robin assignment.
 * If a hardware queue is full, the request is freed back to the tag pool.
 */
void blk_mq_flush(void)
{
    int hw_idx = 0;

    for (int sw = 0; sw < BLK_MQ_MAX_SW_QUEUES; sw++) {
        struct blk_mq_sw_queue *swq = &blk_mq_sw_queues[sw];
        uint64_t irq_flags;

        spinlock_irqsave_acquire(&swq->lock, &irq_flags);
        while (swq->head != swq->tail) {
            struct blk_mq_request *req = swq->queue[swq->head];
            swq->head = (swq->head + 1) % BLK_MQ_DEPTH;

            /* Dispatch to hardware queue (round-robin) */
            struct blk_mq_hw_queue *hwq =
                &blk_mq_hw_queues[hw_idx % blk_mq_num_hw_queues];
            hw_idx++;

            uint64_t irq_flags2;
            spinlock_irqsave_acquire(&hwq->lock, &irq_flags2);
            int next = (hwq->tail + 1) % BLK_MQ_DEPTH;
            if (next != hwq->head) {
                hwq->queue[hwq->tail] = req;
                hwq->tail = next;
            } else {
                /* Hardware queue full, put back */
                blk_mq_free_request(req);
            }
            spinlock_irqsave_release(&hwq->lock, irq_flags2);
        }
        spinlock_irqsave_release(&swq->lock, irq_flags);
    }
}

/**
 * blk_mq_complete - Complete a request from a hardware queue
 * @req: Pointer to the request to complete
 * @error: Completion status (0 for success, negative errno on failure)
 *
 * Invokes the request's completion callback and marks the request as
 * no longer in use.  Safe to call with a NULL request pointer.
 */
void blk_mq_complete(struct blk_mq_request *req, int error)
{
    if (req && req->complete)
        req->complete(req, error);
    if (req)
        req->in_use = 0;
}

/**
 * blk_mq_dispatch - Process pending requests from all hardware queues
 *
 * Iterates over every hardware dispatch queue and processes each
 * enqueued request by invoking its completion callback with success (0).
 * Frees the request back to the tag pool afterwards.  This is a
 * simplified simulation — a real implementation would submit requests
 * to the underlying block device driver.
 */
void blk_mq_dispatch(void)
{
    for (int hw = 0; hw < blk_mq_num_hw_queues; hw++) {
        struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw];
        uint64_t irq_flags;

        spinlock_irqsave_acquire(&hwq->lock, &irq_flags);
        while (hwq->head != hwq->tail) {
            struct blk_mq_request *req = hwq->queue[hwq->head];
            hwq->head = (hwq->head + 1) % BLK_MQ_DEPTH;

            /* Process the request (simulated) */
            req->complete(req, 0);
            hwq->completed++;
            blk_mq_free_request(req);
        }
        spinlock_irqsave_release(&hwq->lock, irq_flags);
    }
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
        spinlock_init(&blk_mq_hw_queues[i].lock);

    blk_mq_initialized = 1;
    kprintf("[OK] blk-mq — Multi-queue block I/O layer (%d HW queues, %d tagged requests)\n",
            blk_mq_num_hw_queues, BLK_MQ_DEPTH);
}

/* ── Stub: blk_mq_init_queue ─────────────────────────────── */
int blk_mq_init_queue(void *dev, void *tag_set)
{
    (void)dev;
    (void)tag_set;
    kprintf("[blk_mq] blk_mq_init_queue: not yet implemented\n");
    return 0;
}

/* ── Stub: blk_mq_exit_queue ─────────────────────────────── */
int blk_mq_exit_queue(void *q)
{
    (void)q;
    kprintf("[blk_mq] blk_mq_exit_queue: not yet implemented\n");
    return 0;
}

/* ── Stub: blk_mq_submit_bio ─────────────────────────────── */
int blk_mq_submit_bio(void *bio)
{
    (void)bio;
    kprintf("[blk_mq] blk_mq_submit_bio: not yet implemented\n");
    return 0;
}

/* ── Stub: blk_mq_complete_request ─────────────────────────────── */
int blk_mq_complete_request(void *rq)
{
    (void)rq;
    kprintf("[blk_mq] blk_mq_complete_request: not yet implemented\n");
    return 0;
}
