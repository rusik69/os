// SPDX-License-Identifier: GPL-2.0-only
/*
 * cake.c — Common Applications Kept Enhanced (comprehensive AQM qdisc)
 *
 * CAKE is a comprehensive AQM and shaping qdisc that combines:
 *   1. Diffserv-aware traffic differentiation (8 tins)
 *   2. Per-flow fair queuing with DRR (Deficit Round Robin)
 *   3. CoDel AQM with ECN marking on a per-flow basis
 *   4. Bandwidth shaping per tin via token buckets
 *
 * The word "CAKE" stands for Common Applications Kept Enhanced,
 * reflecting its goal of improving latency for interactive traffic
 * while maintaining bulk throughput.
 *
 * Reference: https://ieeexplore.ieee.org/document/8475133
 */
#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"
#include "spinlock.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define CAKE_TINS           8       /* 8 traffic classes (diffserv) */
#define CAKE_FLOWS_PER_TIN  256     /* flow buckets per tin */
#define CAKE_FLOW_LIMIT     128     /* max packets per flow */
#define CAKE_QUANTUM        500     /* default DRR quantum (bytes) */
#define CAKE_TARGET_MS      5       /* CoDel target queue delay (ms) */
#define CAKE_INTERVAL_MS    100     /* CoDel interval (ms) */
#define CAKE_DEFAULT_BW     12500000 /* 100 Mbps default (bytes/sec) */
#define CAKE_DEFAULT_ECN    1       /* ECN marking enabled by default */

/* Per-tin bandwidth ratios (fractions of total bandwidth).
 * These approximate the tin priorities in the Linux CAKE default:
 *   Tin 3 (Voice):    25% (highest priority)
 *   Tin 4 (Control):  20%
 *   Tin 2 (Video):    16%
 *   Tin 5 (Critical): 14%
 *   Tin 0 (BE):       10%
 *   Tin 6 (Mgmt):      7%
 *   Tin 1 (Bulk):      5%
 *   Tin 7 (BK):        3% (lowest)
 */
static const uint32_t cake_tin_bandwidth_pct[CAKE_TINS] = {
    10,   /* Tin 0: Best Effort */
    5,    /* Tin 1: Bulk */
    16,   /* Tin 2: Video */
    25,   /* Tin 3: Voice */
    20,   /* Tin 4: Control */
    14,   /* Tin 5: Critical */
    7,    /* Tin 6: Management */
    3,    /* Tin 7: Background */
};

/* ── DSCP to tin lookup table (64 entries indexed by DSCP >> 2) ────
 *
 * Tin assignments (simplified from Linux CAKE):
 *   Tin 0: Best Effort (DSCP 0)
 *   Tin 1: Bulk (CS1)
 *   Tin 2: Video (AF11-43, AF31-33)
 *   Tin 3: Voice (EF, CS7)
 *   Tin 4: Control (CS6, CS5)
 *   Tin 5: Critical (CS4, CS3)
 *   Tin 6: Management (CS2)
 *   Tin 7: Background (default for unclassified)
 */
static const uint8_t cake_dscp_to_tin[64] = {
    /* 0x00-0x07: Best Effort */
    [0x00] = 0, [0x01] = 0, [0x02] = 0, [0x03] = 0,
    [0x04] = 0, [0x05] = 0, [0x06] = 0, [0x07] = 0,
    /* 0x08-0x0f: CS1 (Bulk) / AF11-13 */
    [0x08] = 1, [0x09] = 1, [0x0a] = 2, [0x0b] = 1,
    [0x0c] = 2, [0x0d] = 1, [0x0e] = 2, [0x0f] = 1,
    /* 0x10-0x17: CS2 (Background) */
    [0x10] = 7, [0x11] = 7, [0x12] = 7, [0x13] = 7,
    [0x14] = 7, [0x15] = 7, [0x16] = 7, [0x17] = 7,
    /* 0x18-0x1f: CS3 / AF21-23 */
    [0x18] = 5, [0x19] = 5, [0x1a] = 2, [0x1b] = 5,
    [0x1c] = 2, [0x1d] = 5, [0x1e] = 2, [0x1f] = 5,
    /* 0x20-0x27: CS4 / AF31-33 */
    [0x20] = 5, [0x21] = 5, [0x22] = 5, [0x23] = 5,
    [0x24] = 5, [0x25] = 5, [0x26] = 5, [0x27] = 5,
    /* 0x28-0x2f: CS5 / AF41-43 / EF */
    [0x28] = 4, [0x29] = 4, [0x2a] = 2, [0x2b] = 4,
    [0x2c] = 2, [0x2d] = 4, [0x2e] = 3, [0x2f] = 4,
    /* 0x30-0x37: CS6 (Control) */
    [0x30] = 4, [0x31] = 4, [0x32] = 4, [0x33] = 4,
    [0x34] = 4, [0x35] = 4, [0x36] = 4, [0x37] = 4,
    /* 0x38-0x3f: CS7 (Voice) */
    [0x38] = 3, [0x39] = 3, [0x3a] = 2, [0x3b] = 3,
    [0x3c] = 2, [0x3d] = 3, [0x3e] = 2, [0x3f] = 3,
};

/* ── Per-flow state ─────────────────────────────────────────────── */

struct cake_flow {
    /* Packet queue (ring buffer) */
    void    *queue[CAKE_FLOW_LIMIT];
    int      pkt_len[CAKE_FLOW_LIMIT];
    uint64_t enq_tick[CAKE_FLOW_LIMIT];
    int      head;      /* dequeue index */
    int      tail;      /* enqueue index */
    int      count;     /* packets in queue */

    /* DRR scheduling */
    int      deficit;
    int      quantum;

    /* CoDel state */
    uint64_t first_above_time;
    int      dropping;
    uint64_t drop_next;
    int      dropped;
    int      ecn_marked;
};

/* ── Per-tin state ──────────────────────────────────────────────── */

struct cake_tin {
    struct cake_flow flows[CAKE_FLOWS_PER_TIN];
    int      flow_offset;          /* DRR round-robin start index */
    uint32_t bandwidth;            /* bytes per second for this tin */
    int64_t  tokens;               /* token bucket (Q8 fixed-point) */
    uint64_t last_touched;         /* last token refill tick */
    uint32_t burst;                /* max token accumulation (bytes) */

    /* Statistics */
    uint64_t drops;
    uint64_t marks;
    uint64_t dequeued;
};

/* ── CAKE qdisc private state ───────────────────────────────────── */

struct cake_priv {
    struct cake_tin tins[CAKE_TINS];
    spinlock_t lock;
    int      limit;                /* per-flow queue depth limit */
    int      ecn_enabled;          /* 1 = attempt ECN marking before dropping */
    uint32_t bandwidth;            /* total bandwidth (bytes/sec) */

    /* Aggregate statistics */
    uint64_t total_dropped;
    uint64_t total_ecn_marked;
    uint64_t total_dequeued;
};

/* ── DSCP classification ────────────────────────────────────────── */

/* Extract the DSCP field from an IPv4/IPv6 packet (raw Ethernet frame).
 * Returns the DSCP value (0-63), or 0 if the packet is unparseable. */
static uint8_t cake_extract_dscp(const void *pkt, int len)
{
    const uint8_t *buf = (const uint8_t *)pkt;

    /* Need at least Ethernet (14) + IP header start */
    if (len < 34)
        return 0;

    const uint8_t *ip = buf + 14;
    uint16_t ethertype;

    ethertype = (uint16_t)((buf[12] << 8) | buf[13]);

    /* Skip 802.1Q VLAN tag (4 extra bytes) */
    if (ethertype == 0x8100 && len >= 18) {
        ip = buf + 18;
        if (len < 38)
            return 0;
    }

    /* Extract DSCP (top 6 bits of IP TOS/traffic-class byte) */
    return ip[1] >> 2;
}

/* Classify a DSCP value into a tin index (0..CAKE_TINS-1). */
static inline int cake_classify_tin(uint8_t dscp)
{
    if (dscp >= 64)
        return 0;
    return (int)cake_dscp_to_tin[dscp];
}

/* ── Flow hashing — Jenkins One-At-A-Time ───────────────────────── */

static uint32_t cake_jenkins_hash(const uint8_t *key, int len)
{
    uint32_t hash = 0;

    for (int i = 0; i < len; i++) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

/* Extract a 5-tuple from an Ethernet frame and return a flow bucket
 * index (0 .. CAKE_FLOWS_PER_TIN - 1).
 *
 * Handles IPv4, IPv6, 802.1Q VLAN tags. */
static int cake_flow_hash(const void *pkt, int len)
{
    const uint8_t *buf = (const uint8_t *)pkt;
    uint8_t tuple[13];
    int ti = 0;

    if (len < 34)
        return 0;

    const uint8_t *ip = buf + 14;
    int ip_hdr_len;
    uint16_t ethertype;

    ethertype = (uint16_t)((buf[12] << 8) | buf[13]);
    if (ethertype == 0x8100 && len >= 18) {
        ip = buf + 18;
        if (len < 38)
            return 0;
    }

    uint8_t ip_version = (ip[0] >> 4) & 0x0F;

    if (ip_version == 4) {
        ip_hdr_len = (ip[0] & 0x0F) * 4;
        if (ip_hdr_len < 20 || ip_hdr_len > 60)
            return 0;
        if (len < (int)((ip - buf) + ip_hdr_len + 4))
            return 0;

        uint8_t proto = ip[9];

        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[12 + i];
        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[16 + i];
        tuple[ti++] = proto;

        if ((proto == 6 || proto == 17) &&
            (len >= (int)((ip - buf) + ip_hdr_len + 4))) {
            const uint8_t *l4 = ip + ip_hdr_len;
            tuple[ti++] = l4[0]; tuple[ti++] = l4[1];
            tuple[ti++] = l4[2]; tuple[ti++] = l4[3];
        } else {
            tuple[ti++] = 0; tuple[ti++] = 0;
            tuple[ti++] = 0; tuple[ti++] = 0;
        }
    } else if (ip_version == 6) {
        if (len < (int)((ip - buf) + 40))
            return 0;
        uint8_t proto = ip[6];

        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[8 + i];
        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[24 + i];
        tuple[ti++] = proto;

        if ((proto == 6 || proto == 17) &&
            (len >= (int)((ip - buf) + 44))) {
            const uint8_t *l4 = ip + 40;
            tuple[ti++] = l4[0]; tuple[ti++] = l4[1];
            tuple[ti++] = l4[2]; tuple[ti++] = l4[3];
        } else {
            tuple[ti++] = 0; tuple[ti++] = 0;
            tuple[ti++] = 0; tuple[ti++] = 0;
        }
    } else {
        return ((uintptr_t)pkt >> 4) % CAKE_FLOWS_PER_TIN;
    }

    uint32_t h = cake_jenkins_hash(tuple, ti);
    return (int)(h % CAKE_FLOWS_PER_TIN);
}

/* ── CoDel control law ──────────────────────────────────────────── */

/* Compute the next drop time using the CoDel control law:
 *   next_drop = now + interval / sqrt(count)
 *
 * Uses a lookup table for 1/sqrt(count) for small counts, falling
 * back to a rational approximation.  Returns absolute tick value. */
static uint64_t cake_codel_control_law(uint64_t now, int count,
                                        uint64_t interval_ticks)
{
    uint32_t inv_sqrt;

    switch (count) {
    case 0:  inv_sqrt = 1024; break;
    case 1:  inv_sqrt = 1024; break;
    case 2:  inv_sqrt = 724;  break;
    case 3:  inv_sqrt = 591;  break;
    case 4:  inv_sqrt = 512;  break;
    case 5:  inv_sqrt = 458;  break;
    case 6:  inv_sqrt = 418;  break;
    case 7:  inv_sqrt = 387;  break;
    case 8:  inv_sqrt = 362;  break;
    case 9:  inv_sqrt = 341;  break;
    case 10: inv_sqrt = 324;  break;
    default:
        {
            uint32_t c = (uint32_t)(count > 65536 ? 65536 : count);
            inv_sqrt = (1024u * 1024u) / (c * 4u);
            if (inv_sqrt < 32)
                inv_sqrt = 32;
            if (inv_sqrt > 1024)
                inv_sqrt = 1024;
        }
        break;
    }

    uint64_t delay = (interval_ticks * (uint64_t)inv_sqrt) >> 10;
    if (delay < 1)
        delay = 1;
    return now + delay;
}

/* ── CoDel drop decision ────────────────────────────────────────── */

/* Decide whether to drop (or ECN-mark) the head packet of a flow.
 *
 * CoDel algorithm (per-flow):
 *   1. Compute sojourn time = now - head-packet-enqueue-tick
 *   2. If sojourn < target: clear first_above_time, return 0
 *   3. If sojourn >= target and first_above_time == 0:
 *      record first_above_time = now, return 0
 *   4. If sojourn >= target and interval has elapsed since
 *      first_above_time, enter dropping state and return 1
 *
 * Returns 1 if the head packet should be dropped/marked, 0 otherwise. */
static int cake_codel_should_drop(struct cake_flow *flow, uint64_t now,
                                   uint64_t target_ticks,
                                   uint64_t interval_ticks)
{
    if (flow->count == 0)
        return 0;

    /* Sojourn time in ticks since enqueue */
    uint64_t sojourn_ticks = now - flow->enq_tick[flow->head];

    if (sojourn_ticks < target_ticks) {
        flow->first_above_time = 0;
        return 0;
    }

    if (flow->first_above_time == 0) {
        flow->first_above_time = now;
        return 0;
    }

    uint64_t above_ticks = now - flow->first_above_time;
    if (above_ticks >= interval_ticks) {
        flow->drop_next = cake_codel_control_law(now,
                            flow->dropped + 1, interval_ticks);
        return 1;
    }

    return 0;
}

/* ── ECN marking ────────────────────────────────────────────────── */

/* Mark a packet with ECN Congestion Experienced (CE) codepoint.
 * Returns 1 if marked, 0 if the packet is not ECN-capable. */
static int cake_ecn_mark(void *pkt, int len)
{
    uint8_t *buf = (uint8_t *)pkt;
    uint8_t *ip;
    uint16_t ethertype;

    if (len < 14)
        return 0;

    ethertype = (uint16_t)((buf[12] << 8) | buf[13]);

    if (ethertype == 0x8100 && len >= 18) {
        ip = buf + 18;
        if (len < 38)
            return 0;
    } else {
        ip = buf + 14;
        if (len < 34)
            return 0;
    }

    uint8_t ip_version = (ip[0] >> 4) & 0x0F;
    if (ip_version != 4 && ip_version != 6)
        return 0;

    uint8_t ecn = ip[1] & 0x03;
    /* 00=Non-ECT, 01=ECT(1), 10=ECT(0), 11=CE */
    if (ecn == 0x00 || ecn == 0x03)
        return 0;

    /* Set CE codepoint */
    ip[1] |= 0x03;
    return 1;
}

/* ── Token bucket helpers (per-tin bandwidth shaping) ───────────── */

/* Refill tokens for a tin based on elapsed time.
 * TIMER_FREQ = 100 Hz → 1 tick = 10 ms. */
static void cake_refill_tin(struct cake_tin *tin, uint64_t now)
{
    if (now <= tin->last_touched)
        return;

    uint64_t elapsed = now - tin->last_touched;
    /* Rate: add (elapsed * bandwidth / TIMER_FREQ) bytes worth of tokens */
    int64_t add = (int64_t)(elapsed * (uint64_t)tin->bandwidth) / TIMER_FREQ;
    tin->tokens += add << 8;
    if (tin->tokens > (int64_t)tin->burst << 8)
        tin->tokens = (int64_t)tin->burst << 8;

    tin->last_touched = now;
}

/* Check whether a tin can send a packet of @len bytes.
 * Returns 1 if allowed, 0 if throttled. */
static int cake_tin_can_send(struct cake_tin *tin, int len)
{
    int charge = len < 64 ? 64 : len; /* minimum packet unit = 64 */
    return (tin->tokens >= ((int64_t)charge << 8)) ? 1 : 0;
}

/* Discount tokens after sending a packet. */
static void cake_tin_consume(struct cake_tin *tin, int len)
{
    int charge = len < 64 ? 64 : len;
    tin->tokens -= (int64_t)charge << 8;
}

/* ── qdisc enqueue callback ─────────────────────────────────────── */

static int cake_enqueue(struct qdisc *q, void *pkt, int len)
{
    struct cake_priv *priv;
    uint8_t dscp;
    int tin_idx, flow_idx;
    struct cake_tin *tin;
    struct cake_flow *flow;

    if (!q || !pkt)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    /* Extract DSCP and classify into a tin */
    dscp = cake_extract_dscp(pkt, len);
    tin_idx = cake_classify_tin((uint8_t)dscp);

    /* Hash packet to a flow within the tin */
    flow_idx = cake_flow_hash(pkt, len);

    tin = &priv->tins[tin_idx];
    flow = &tin->flows[flow_idx];

    /* Check per-flow queue limit */
    if (flow->count >= priv->limit) {
        priv->total_dropped++;
        tin->drops++;
        return -ENOBUFS;
    }

    /* Store packet and metadata */
    flow->queue[flow->tail] = pkt;
    flow->pkt_len[flow->tail] = len;
    flow->enq_tick[flow->tail] = timer_get_ticks();
    flow->tail = (flow->tail + 1) % CAKE_FLOW_LIMIT;
    flow->count++;

    return 0;
}

/* ── qdisc dequeue callback ─────────────────────────────────────── */

static void *cake_dequeue(struct qdisc *q)
{
    struct cake_priv *priv;
    uint64_t now;
    uint64_t target_ticks, interval_ticks;
    uint64_t irq_flags;

    if (!q)
        return NULL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return NULL;

    spinlock_irqsave_acquire(&priv->lock, &irq_flags);

    now = timer_get_ticks();
    target_ticks   = CAKE_TARGET_MS / 10;
    interval_ticks = CAKE_INTERVAL_MS / 10;
    if (target_ticks < 1)
        target_ticks = 1;
    if (interval_ticks < 1)
        interval_ticks = 1;

    /* Scan tins in priority order (0=highest priority) */
    for (int t = 0; t < CAKE_TINS; t++) {
        struct cake_tin *tin = &priv->tins[t];

        /* Bandwidth shaping: refill tokens and check if tin can send */
        cake_refill_tin(tin, now);
        if (!cake_tin_can_send(tin, 0)) {
            /* Tin is shaped off — check for bulk-send allowance:
             * even a shaped tin may burst one packet if it waited. */
            if (tin->tokens < 0)
                continue;
        }

        /* DRR scan within this tin */
        int start = tin->flow_offset;

        for (int i = 0; i < CAKE_FLOWS_PER_TIN; i++) {
            int b = (start + i) % CAKE_FLOWS_PER_TIN;
            struct cake_flow *flow = &tin->flows[b];

            if (flow->count == 0) {
                flow->deficit = 0;
                continue;
            }

            /* DRR: refresh deficit */
            if (flow->deficit <= 0)
                flow->deficit += flow->quantum;

            int head_len = flow->pkt_len[flow->head];

            /* DRR: skip if head packet exceeds deficit */
            if (head_len > flow->deficit)
                continue;

            /* ── CoDel AQM ──────────────────────────────────── */

            if (cake_codel_should_drop(flow, now,
                                       target_ticks, interval_ticks)) {
                /* Try ECN marking first if enabled */
                if (priv->ecn_enabled &&
                    cake_ecn_mark(flow->queue[flow->head],
                                  flow->pkt_len[flow->head])) {
                    flow->ecn_marked++;
                    priv->total_ecn_marked++;
                    tin->marks++;
                    /* ECN-marked packet still dequeued below */
                } else {
                    /* Drop the head packet */
                    flow->head = (flow->head + 1) % CAKE_FLOW_LIMIT;
                    flow->count--;
                    flow->dropped++;
                    priv->total_dropped++;
                    tin->drops++;
                    /* Re-check new head */
                    if (flow->count == 0)
                        continue;
                    if (cake_codel_should_drop(flow, now,
                                               target_ticks,
                                               interval_ticks)) {
                        flow->head = (flow->head + 1) % CAKE_FLOW_LIMIT;
                        flow->count--;
                        flow->dropped++;
                        priv->total_dropped++;
                        tin->drops++;
                        continue;
                    }
                    head_len = flow->pkt_len[flow->head];
                }
            }

            /* DRR: deduct from deficit */
            flow->deficit -= head_len;

            /* Token bucket: consume bandwidth tokens */
            cake_tin_consume(tin, head_len);

            /* Dequeue */
            void *pkt = flow->queue[flow->head];
            flow->head = (flow->head + 1) % CAKE_FLOW_LIMIT;
            flow->count--;
            priv->total_dequeued++;
            tin->dequeued++;

            /* Advance round-robin pointer */
            tin->flow_offset = (b + 1) % CAKE_FLOWS_PER_TIN;

            spinlock_irqsave_release(&priv->lock, irq_flags);
            return pkt;
        }
    }

    spinlock_irqsave_release(&priv->lock, irq_flags);
    return NULL;
}

/* ── qdisc drop callback ────────────────────────────────────────── */

/* Drop one packet from the longest queue across all tins.
 * Returns 0 on success, -ENOENT if no packets to drop. */
static int cake_drop(struct qdisc *q)
{
    struct cake_priv *priv;
    int longest_tin = -1;
    int longest_flow = -1;
    int longest_count = 0;

    if (!q)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    /* Find the longest queue across all tins */
    for (int t = 0; t < CAKE_TINS; t++) {
        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++) {
            if (priv->tins[t].flows[f].count > longest_count) {
                longest_count = priv->tins[t].flows[f].count;
                longest_tin = t;
                longest_flow = f;
            }
        }
    }

    if (longest_flow < 0 || longest_count == 0)
        return -ENOENT;

    struct cake_flow *flow = &priv->tins[longest_tin].flows[longest_flow];

    flow->head = (flow->head + 1) % CAKE_FLOW_LIMIT;
    flow->count--;
    flow->dropped++;
    priv->total_dropped++;
    priv->tins[longest_tin].drops++;
    return 0;
}

/* ── qdisc reset ────────────────────────────────────────────────── */

static int cake_reset(struct qdisc *q)
{
    struct cake_priv *priv;

    if (!q)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    /* Reset all flows across all tins */
    for (int t = 0; t < CAKE_TINS; t++) {
        struct cake_tin *tin = &priv->tins[t];

        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++) {
            struct cake_flow *flow = &tin->flows[f];

            flow->head = 0;
            flow->tail = 0;
            flow->count = 0;
            flow->deficit = 0;
            flow->first_above_time = 0;
            flow->dropping = 0;
            flow->drop_next = 0;
        }
        tin->flow_offset = 0;
    }

    return 0;
}

/* ── qdisc configuration change ─────────────────────────────────── */

static int cake_change(struct qdisc *q, void *cfg)
{
    struct cake_priv *priv;
    const struct cake_spec *spec;

    if (!q || !cfg)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    spec = (const struct cake_spec *)cfg;

    /* Update total bandwidth (redistributes to tins based on ratios) */
    if (spec->bandwidth > 0) {
        priv->bandwidth = spec->bandwidth;
        for (int t = 0; t < CAKE_TINS; t++) {
            uint64_t tin_bw = (uint64_t)priv->bandwidth *
                              cake_tin_bandwidth_pct[t] / 100;
            if (tin_bw < 1000)
                tin_bw = 1000; /* minimum 1 Kbps */
            priv->tins[t].bandwidth = (uint32_t)tin_bw;
            /* Auto-compute burst as bandwidth / 160 (≈ 100ms worth) */
            priv->tins[t].burst = (uint32_t)(tin_bw / 160);
            if (priv->tins[t].burst < 1500)
                priv->tins[t].burst = 1500; /* at least 1 MTU */
        }
    }

    /* Update per-flow queue limit */
    if (spec->limit > 0) {
        if (spec->limit > CAKE_FLOW_LIMIT)
            priv->limit = CAKE_FLOW_LIMIT;
        else
            priv->limit = (int)spec->limit;
    }

    /* Update ECN setting */
    priv->ecn_enabled = spec->ecn ? 1 : 0;

    return 0;
}

/* ── Dump statistics ────────────────────────────────────────────── */

static int cake_dump(struct qdisc *q, char *buf, size_t buf_len)
{
    struct cake_priv *priv;
    int total_active;
    int total_qlen;
    int n;

    if (!q || !buf || buf_len == 0)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    /* Count active flows and total queue length */
    total_active = 0;
    total_qlen = 0;

    for (int t = 0; t < CAKE_TINS; t++) {
        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++) {
            int cnt = priv->tins[t].flows[f].count;

            if (cnt > 0) {
                total_active++;
                total_qlen += cnt;
            }
        }
    }

    n = snprintf(buf, buf_len,
        "cake:\n"
        "  bandwidth:  %u bytes/s\n"
        "  limit:      %d packets/flow\n"
        "  ecn:        %s\n"
        "  tins:       %d\n"
        "  flows:      %d active / %d total\n"
        "  dropped:    %llu\n"
        "  ecn_marked: %llu\n"
        "  dequeued:   %llu\n"
        "  qlen:       %d packets\n",
        priv->bandwidth,
        priv->limit,
        priv->ecn_enabled ? "enabled" : "disabled",
        CAKE_TINS,
        total_active, CAKE_TINS * CAKE_FLOWS_PER_TIN,
        (unsigned long long)priv->total_dropped,
        (unsigned long long)priv->total_ecn_marked,
        (unsigned long long)priv->total_dequeued,
        total_qlen);

    return n;
}

static int cake_dump_stats(struct qdisc *q, struct cake_stats *stats)
{
    struct cake_priv *priv;

    if (!q || !stats)
        return -EINVAL;

    priv = (struct cake_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    memset(stats, 0, sizeof(*stats));

    stats->total_bandwidth = priv->bandwidth;
    stats->limit = priv->limit;
    stats->ecn_enabled = priv->ecn_enabled;

    for (int t = 0; t < CAKE_TINS; t++) {
        struct cake_tin *tin = &priv->tins[t];

        stats->tin_drops[t] = tin->drops;
        stats->tin_marks[t] = tin->marks;
        stats->tin_dequeued[t] = tin->dequeued;

        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++) {
            int cnt = priv->tins[t].flows[f].count;

            if (cnt > 0) {
                stats->tin_flows[t]++;
                stats->tin_qlen[t] += cnt;
            }
        }
    }

    return 0;
}

/* ── qdisc create ───────────────────────────────────────────────── */

/* Forward declaration of stats callback */
static void cake_fill_stats(struct qdisc *q, struct tc_stats *st);

struct qdisc *cake_create(const struct cake_spec *spec)
{
    struct qdisc *q;
    struct cake_priv *priv;

    /* Allocate qdisc */
    q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q)
        return NULL;

    /* Allocate private state */
    priv = (struct cake_priv *)kmalloc(sizeof(struct cake_priv));
    if (!priv) {
        kfree(q);
        return NULL;
    }

    memset(priv, 0, sizeof(struct cake_priv));
    spinlock_init(&priv->lock);

    /* Default configuration */
    if (spec && spec->bandwidth > 0)
        priv->bandwidth = spec->bandwidth;
    else
        priv->bandwidth = CAKE_DEFAULT_BW;

    if (spec && spec->limit > 0) {
        if (spec->limit > CAKE_FLOW_LIMIT)
            priv->limit = CAKE_FLOW_LIMIT;
        else
            priv->limit = (int)spec->limit;
    } else {
        priv->limit = 64;
    }

    priv->ecn_enabled = (spec && spec->ecn) ? 1 : CAKE_DEFAULT_ECN;

    /* Initialise per-tin bandwidth allocation based on ratios */
    for (int t = 0; t < CAKE_TINS; t++) {
        struct cake_tin *tin = &priv->tins[t];
        uint64_t tin_bw;

        tin_bw = (uint64_t)priv->bandwidth *
                 cake_tin_bandwidth_pct[t] / 100;
        if (tin_bw < 1000)
            tin_bw = 1000;
        tin->bandwidth = (uint32_t)tin_bw;

        /* Auto-compute burst as ~100ms worth of bandwidth */
        tin->burst = (uint32_t)(tin_bw / 160);
        if (tin->burst < 1500)
            tin->burst = 1500;

        tin->last_touched = timer_get_ticks();
        tin->tokens = (int64_t)tin->burst << 8;

        /* Initialise per-flow quantum */
        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++)
            tin->flows[f].quantum = CAKE_QUANTUM;
    }

    /* Wire qdisc operations */
    q->type    = QDISC_CAKE;
    q->priv    = priv;
    q->enqueue = cake_enqueue;
    q->dequeue = cake_dequeue;
    q->drop    = cake_drop;
    q->get_stats      = cake_fill_stats;
    q->get_class_stats = NULL;

    kprintf("[OK] CAKE qdisc — Common Applications Kept Enhanced\n");
    return q;
}

/* ── Statistics callback ────────────────────────────────────────── */

static void cake_fill_stats(struct qdisc *q, struct tc_stats *st)
{
    struct cake_priv *priv = (struct cake_priv *)q->priv;
    if (!priv || !st) return;
    memset(st, 0, sizeof(*st));
    uint32_t drops = 0, marks = 0;
    uint32_t qlen = 0;
    for (int t = 0; t < CAKE_TINS; t++) {
        struct cake_tin *tin = &priv->tins[t];
        drops += (uint32_t)tin->drops;
        marks += (uint32_t)tin->marks;
        for (int f = 0; f < CAKE_FLOWS_PER_TIN; f++)
            qlen += (uint32_t)tin->flows[f].count;
    }
    st->drops      = drops;
    st->overlimits = marks;
    st->qlen       = qlen;
    st->backlog    = qlen * 1500;
}

/* ── Module initialisation ──────────────────────────────────────── */

void cake_init(void)
{
    kprintf("[OK] CAKE — Common Applications Kept Enhanced (comprehensive AQM)\n");
}

#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("CAKE: Common Applications Kept Enhanced — comprehensive AQM qdisc");
MODULE_AUTHOR("OS Kernel Team");
module_init(cake_init);
