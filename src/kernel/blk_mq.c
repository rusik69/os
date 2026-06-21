// SPDX-License-Identifier: GPL-2.0-only
/*
 * blk_mq.c — Multi-queue block I/O layer
 *
 * Implements a multi-queue block layer with per-CPU submission queues,
 * software staging queues, and hardware dispatch queues.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "smp.h"
#include "heap.h"

#define BLK_MQ_MAX_HW_QUEUES   16
#define BLK_MQ_MAX_SW_QUEUES   64
#define BLK_MQ_DEPTH           256

/* I/O request */
struct blk_mq_request {
    int in_use;
    uint64_t sector;
    uint8_t *buffer;
    size_t count;
    int write; /* 0=read, 1=write */
    void (*complete)(struct blk_mq_request *req, int error);
};

/* Software staging queue (per-CPU) */
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

static struct blk_mq_sw_queue blk_mq_sw_queues[BLK_MQ_MAX_SW_QUEUES];
static struct blk_mq_hw_queue blk_mq_hw_queues[BLK_MQ_MAX_HW_QUEUES];
static int blk_mq_num_hw_queues;
static int blk_mq_initialized;

/* Allocate a request from the pool */
static struct blk_mq_request *blk_mq_alloc_request(void)
{
    /* Simplified: kmalloc each request */
    return (struct blk_mq_request *)kmalloc(sizeof(struct blk_mq_request));
}

/* Free a request */
static void blk_mq_free_request(struct blk_mq_request *req)
{
    kfree(req);
}

/* Submit I/O request through the multi-queue layer */
int blk_mq_submit_request(uint64_t sector, uint8_t *buffer,
                           size_t count, int write,
                           void (*complete)(struct blk_mq_request *, int))
{
    struct blk_mq_request *req;
    int cpu = smp_get_cpu_id();

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
    if (cpu >= BLK_MQ_MAX_SW_QUEUES) cpu = 0;
    struct blk_mq_sw_queue *swq = &blk_mq_sw_queues[cpu];

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&swq->lock, &irq_flags);
    int next = (swq->tail + 1) % BLK_MQ_DEPTH;
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

/* Flush software queues to hardware queues */
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
            struct blk_mq_hw_queue *hwq = &blk_mq_hw_queues[hw_idx % blk_mq_num_hw_queues];
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

/* Complete a request from a hardware queue */
void blk_mq_complete(struct blk_mq_request *req, int error)
{
    if (req && req->complete)
        req->complete(req, error);
    if (req)
        req->in_use = 0;
}

/* Start hardware dispatch */
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

void blk_mq_init(void)
{
    if (blk_mq_initialized)
        return;

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
    kprintf("[OK] blk-mq — Multi-queue block I/O layer (%d HW queues)\n",
            blk_mq_num_hw_queues);
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
