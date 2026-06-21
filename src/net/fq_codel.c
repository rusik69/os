// SPDX-License-Identifier: GPL-2.0-only
/*
 * fq_codel.c — Fair Queuing with Controlled Delay (AQM packet scheduler)
 *
 * Implements the FQ-CoDel active queue management algorithm.
 * Provides per-flow queuing with a stochastic flow classifier
 * and CoDel AQM for each queue.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

#define FQ_CODEL_QUEUES      1024
#define FQ_CODEL_DEFAULT_LIMIT 128
#define FQ_CODEL_INTERVAL    100  /* 100ms in timer ticks */
#define FQ_CODEL_TARGET      5    /* 5ms target */

struct fq_codel_flow {
    uint8_t  queue[65536];
    uint16_t head;
    uint16_t tail;
    int      count;
    uint64_t dropping;       /* 1 if in dropping state */
    uint64_t first_above_time;
    uint64_t drop_next;
    int      backlog;
    uint64_t enq_timestamp;  /* sojourn time: timestamp of the packet at head */
};

struct fq_codel_sched {
    struct fq_codel_flow flows[FQ_CODEL_QUEUES];
    spinlock_t lock;
    int quantum;
    int limit;
};

static struct fq_codel_sched fq_codel;

/* Simple hash for flow classification */
static uint32_t fq_codel_hash(uint32_t src_ip, uint32_t dst_ip,
                               uint16_t src_port, uint16_t dst_port,
                               uint8_t proto)
{
    uint32_t h = src_ip ^ dst_ip ^ ((uint32_t)proto << 16) ^
                 ((uint32_t)src_port << 16) ^ dst_port;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & (FQ_CODEL_QUEUES - 1);
}

/* CoDel control law - next drop time */
static uint64_t fq_codel_control_law(uint64_t next, uint32_t count, uint64_t interval)
{
    return next + (interval / (uint64_t)count);
}

/* Enqueue a packet */
int fq_codel_enqueue(uint32_t src_ip, uint32_t dst_ip,
                      uint16_t src_port, uint16_t dst_port,
                      uint8_t proto, const uint8_t *pkt, size_t len)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&fq_codel.lock, &irq_flags);

    uint32_t idx = fq_codel_hash(src_ip, dst_ip, src_port, dst_port, proto);
    struct fq_codel_flow *flow = &fq_codel.flows[idx];

    if (flow->count >= fq_codel.limit) {
        spinlock_irqsave_release(&fq_codel.lock, irq_flags);
        return -ENOBUFS; /* drop tail */
    }

    /* Copy packet into queue (simplified ring buffer) */
    if (flow->tail + len > sizeof(flow->queue)) {
        spinlock_irqsave_release(&fq_codel.lock, irq_flags);
        return -ENOBUFS;
    }

    memcpy(flow->queue + flow->tail, pkt, len);
    flow->tail += (uint16_t)len;
    flow->count++;
    flow->backlog += (int)len;

    /* Record enqueue timestamp for sojourn time tracking */
    if (flow->count == 1)
        flow->enq_timestamp = timer_get_ticks();

    spinlock_irqsave_release(&fq_codel.lock, irq_flags);
    return 0;
}

/* Dequeue a packet with CoDel AQM */
int fq_codel_dequeue(uint8_t *pkt, size_t *max_len)
{
    uint64_t irq_flags;
    uint64_t now;
    uint64_t sojourn;

    spinlock_irqsave_acquire(&fq_codel.lock, &irq_flags);

    /* Find the flow with the smallest queue (Fair Queuing) */
    int min_idx = -1;
    int min_backlog = 0x7FFFFFFF;

    for (int i = 0; i < FQ_CODEL_QUEUES; i++) {
        if (fq_codel.flows[i].count > 0 &&
            fq_codel.flows[i].backlog < min_backlog) {
            min_backlog = fq_codel.flows[i].backlog;
            min_idx = i;
        }
    }

    if (min_idx < 0) {
        spinlock_irqsave_release(&fq_codel.lock, irq_flags);
        return 0; /* no packets */
    }

    struct fq_codel_flow *flow = &fq_codel.flows[min_idx];

    /* Dequeue the packet */
    size_t pkt_len = flow->tail - flow->head;
    if (pkt_len > *max_len) pkt_len = *max_len;
    memcpy(pkt, flow->queue + flow->head, pkt_len);
    flow->head += (uint16_t)pkt_len;
    flow->count--;
    flow->backlog -= (int)pkt_len;
    *max_len = pkt_len;

    /* Update enq_timestamp to the next packet in queue if any */
    if (flow->count > 0) {
        /* In a simple flat queue, we approximate by timestamping at dequeue time.
         * A full implementation would store per-packet timestamps. */
        flow->enq_timestamp = timer_get_ticks();
    }

    /* CoDel AQM */
    now = timer_get_ticks();
    sojourn = now - flow->enq_timestamp;  /* actual sojourn time */

    if (sojourn > FQ_CODEL_TARGET) {
        if (!flow->dropping) {
            flow->dropping = 1;
            flow->drop_next = fq_codel_control_law(now, flow->count,
                                                     FQ_CODEL_INTERVAL);
        }
        if (now >= flow->drop_next) {
            /* Mark/drop */
            flow->count++;
            flow->drop_next = fq_codel_control_law(flow->drop_next,
                                                     flow->count,
                                                     FQ_CODEL_INTERVAL);
            /* In real implementation, we'd drop this packet */
        }
    } else {
        flow->dropping = 0;
    }

    spinlock_irqsave_release(&fq_codel.lock, irq_flags);
    return (int)pkt_len;
}

void fq_codel_init(void)
{
    memset(&fq_codel, 0, sizeof(fq_codel));
    spinlock_init(&fq_codel.lock);
    fq_codel.quantum = 1500; /* MTU-sized quantum */
    fq_codel.limit = FQ_CODEL_DEFAULT_LIMIT;
    kprintf("[OK] FQ-CoDel — Fair Queuing with Controlled Delay\n");
}
#include "module.h"
module_init(fq_codel_init);

/* ── Stub: fq_codel_reset ─────────────────────────────── */
int fq_codel_reset(void *sch)
{
    (void)sch;
    kprintf("[fq_codel] fq_codel_reset: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: fq_codel_destroy ─────────────────────────────── */
int fq_codel_destroy(void *sch)
{
    (void)sch;
    kprintf("[fq_codel] fq_codel_destroy: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: fq_codel_change ─────────────────────────────── */
int fq_codel_change(void *sch, void *cfg)
{
    (void)sch;
    (void)cfg;
    kprintf("[fq_codel] fq_codel_change: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: fq_codel_dump ─────────────────────────────── */
int fq_codel_dump(void *sch, void *skb)
{
    (void)sch;
    (void)skb;
    kprintf("[fq_codel] fq_codel_dump: not yet implemented\n");
    return -ENOSYS;
}
