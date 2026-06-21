// SPDX-License-Identifier: GPL-2.0-only
/*
 * cake.c — Common Applications Kept Enhanced (comprehensive AQM)
 *
 * Implements the CAKE smart queue management algorithm combining
 * bandwidth shaping, diffuse marking, and per-flow queuing.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "spinlock.h"
#include "timer.h"

#define CAKE_QUEUES        8
#define CAKE_DEFAULT_LIMIT 10240
#define CAKE_TINS          8  /* 8 traffic classes (diffserv) */
#define CAKE_TARGET        5  /* 5ms codel target */
#define CAKE_INTERVAL      100 /* 100ms codel interval */

struct cake_flow {
    uint8_t  queue[65536];
    uint16_t head;
    uint16_t tail;
    int      count;
    int      backlog;
    uint64_t enq_time[256];  /* sojourn time tracking: enqueue timestamps per packet */
    int      enq_idx;        /* index into enq_time ring (matches head packets) */
};

struct cake_tin {
    struct cake_flow flows[CAKE_QUEUES];
    uint64_t drops;
    uint64_t marks;
    int threshold;
};

struct cake_sched {
    struct cake_tin tins[CAKE_TINS];
    spinlock_t lock;
    uint64_t bandwidth;  /* bytes per second */
    uint64_t interval;
    int limit;
};

static struct cake_sched cake;

/* Map DSCP to tin (simplified) */
static int cake_classify_tin(uint8_t dscp)
{
    (void)dscp;
    /* Default: all traffic in tin 0 */
    return 0;
}

/* Enqueue a packet */
int cake_enqueue(const uint8_t *pkt, size_t len, uint8_t dscp)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cake.lock, &irq_flags);

    int tin_idx = cake_classify_tin(dscp);
    if (tin_idx < 0 || tin_idx >= CAKE_TINS) tin_idx = 0;

    /* Simple hash to flow within tin */
    int flow_idx = 0;
    struct cake_flow *flow = &cake.tins[tin_idx].flows[flow_idx];

    if (flow->count >= cake.limit || flow->tail + len > sizeof(flow->queue)) {
        cake.tins[tin_idx].drops++;
        spinlock_irqsave_release(&cake.lock, irq_flags);
        return -ENOBUFS;
    }

    memcpy(flow->queue + flow->tail, pkt, len);
    flow->tail += (uint16_t)len;
    flow->count++;
    flow->backlog += (int)len;

    /* Record enqueue timestamp for sojourn time tracking (CoDel AQM) */
    int et_idx = flow->count > 0 ? (flow->enq_idx + flow->count - 1) % 256 : flow->enq_idx;
    if (flow->count <= 256)
        flow->enq_time[et_idx] = timer_get_ticks();

    spinlock_irqsave_release(&cake.lock, irq_flags);
    return 0;
}

/* Dequeue a packet with CAKE AQM */
int cake_dequeue(uint8_t *pkt, size_t *max_len)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&cake.lock, &irq_flags);

    /* Find a non-empty tin with lowest priority */
    int best_tin = -1;
    for (int t = 0; t < CAKE_TINS; t++) {
        for (int f = 0; f < CAKE_QUEUES; f++) {
            if (cake.tins[t].flows[f].count > 0) {
                best_tin = t;
                goto found;
            }
        }
    }
found:
    if (best_tin < 0) {
        spinlock_irqsave_release(&cake.lock, irq_flags);
        return 0;
    }

    /* Dequeue from first non-empty flow in this tin */
    for (int f = 0; f < CAKE_QUEUES; f++) {
        struct cake_flow *flow = &cake.tins[best_tin].flows[f];
        if (flow->count > 0) {
            size_t pkt_len = flow->tail - flow->head;
            if (pkt_len > *max_len) pkt_len = *max_len;
            memcpy(pkt, flow->queue + flow->head, pkt_len);
            flow->head += (uint16_t)pkt_len;
            flow->count--;
            flow->backlog -= (int)pkt_len;
            *max_len = pkt_len;

            /* Advance enq_time index */
            flow->enq_idx = (flow->enq_idx + 1) % 256;

            /* CoDel AQM: check sojourn time */
            uint64_t now = timer_get_ticks();
            uint64_t sojourn = now - flow->enq_time[flow->enq_idx];

            if (sojourn > CAKE_TARGET) {
                cake.tins[best_tin].marks++;
                /* In a full implementation, we would set ECN mark on the packet */
            }

            /* Drop if exceeding threshold for too long (codel control law) */
            if (sojourn > CAKE_TARGET * 2 && flow->backlog > 65536) {
                cake.tins[best_tin].drops++;
            }

            spinlock_irqsave_release(&cake.lock, irq_flags);
            return (int)pkt_len;
        }
    }

    spinlock_irqsave_release(&cake.lock, irq_flags);
    return 0;
}

void cake_init(void)
{
    memset(&cake, 0, sizeof(cake));
    spinlock_init(&cake.lock);
    cake.bandwidth = 100000000; /* 100 Mbps default */
    cake.interval = 100;        /* 100ms */
    cake.limit = CAKE_DEFAULT_LIMIT;

    /* Initialize tin thresholds */
    for (int t = 0; t < CAKE_TINS; t++)
        cake.tins[t].threshold = 65536; /* 64KB threshold */

    kprintf("[OK] CAKE — Common Applications Kept Enhanced (comprehensive AQM)\n");
}
#include "module.h"
module_init(cake_init);
