// SPDX-License-Identifier: GPL-2.0-only
/*
 * fq_codel.c — Fair Queuing with Controlled Delay (AQM packet scheduler)
 *
 * Implements the FQ-CoDel active queue management algorithm from
 * "Controlled Delay Active Queue Management" by Kathleen Nichols and
 * Van Jacobson (2012).  Combines per-flow fair queuing (FQ) with the
 * CoDel AQM to provide low latency, fair bandwidth sharing, and
 * robust behaviour under overload.
 *
 * Each packet flow (identified by a 5-tuple hash) gets an independent
 * FIFO queue.  The Deficit Round Robin (DRR) scheduler ensures no
 * single flow can monopolise the link.  CoDel tracks the per-packet
 * sojourn time (enqueue → dequeue interval) for the head packet of
 * each flow and drops (or ECN-marks) when the minimum sojourn time
 * exceeds the target for longer than the interval.
 *
 * Reference: https://datatracker.ietf.org/doc/rfc8290/
 *            https://dl.acm.org/doi/10.1145/2069331.2069332
 */
#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define FQ_CODEL_FLOWS        256     /* number of flow buckets (power of 2) */
#define FQ_CODEL_LIMIT        1024    /* max packets per flow */
#define FQ_CODEL_QUANTUM      300     /* default DRR quantum (bytes) */
#define FQ_CODEL_TARGET_MS    5       /* CoDel target queuing delay (ms) */
#define FQ_CODEL_INTERVAL_MS  100     /* CoDel interval (ms) */
#define FQ_CODEL_DEFAULT_ECN  1       /* ECN marking enabled by default */

/* ── Per-flow state ─────────────────────────────────────────────── */

struct fq_codel_flow {
    /* Packet queue (ring buffer of packet pointers) */
    void    *queue[FQ_CODEL_LIMIT];   /* packet pointers (owned by caller) */
    int      pkt_len[FQ_CODEL_LIMIT]; /* packet lengths */
    uint64_t enq_tick[FQ_CODEL_LIMIT];/* enqueue timestamp (ticks) for sojourn */
    int      head;                    /* dequeue index */
    int      tail;                    /* enqueue index */
    int      count;                   /* packets in this flow */

    /* DRR scheduling */
    int      deficit;                 /* DRR deficit counter (bytes) */
    int      quantum;                 /* DRR quantum for this flow */

    /* CoDel state (per-flow controlled delay) */
    uint64_t first_above_time;        /* tick when sojourn first exceeded target */
    int      dropping;                /* 1 = in dropping state */
    uint64_t drop_next;               /* next drop time (absolute ticks) */
    int      dropped;                 /* lifetime drops from this flow */
    int      ecn_marked;              /* lifetime ECN marks from this flow */
};

/* ── FQ-CoDel private state ─────────────────────────────────────── */

struct fq_codel_priv {
    struct fq_codel_flow flows[FQ_CODEL_FLOWS];
    int      quantum;                 /* default DRR quantum for new flows */
    int      flow_offset;             /* DRR round-robin scan start index */
    int      limit;                   /* per-flow queue depth limit */
    int      ecn_enabled;             /* 1 = attempt ECN marking before dropping */

    /* Aggregate statistics */
    uint64_t total_dropped;
    uint64_t total_ecn_marked;
    uint64_t total_dequeued;
};

/* Static instance (compiled both as built-in kernel object and as .ko) */
static struct fq_codel_priv fq_codel;

/* ── Flow hash — Jenkins One-At-A-Time ────────────────────────────── */

/* Jenkins one-at-a-time hash over a byte buffer.  Returns a 32-bit
 * hash value suitable for modulo into the flow table. */
static uint32_t jenkins_hash(const uint8_t *key, int len)
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
 * index (0 .. FQ_CODEL_FLOWS - 1).
 *
 * Handles:
 *   - IPv4 (IHL, protocol, src/dst addr)
 *   - IPv6 (next-header, src/dst addr truncated to 4+4 bytes)
 *   - 802.1Q VLAN tags (EtherType 0x8100)
 *   - TCP/UDP transport ports
 *
 * Returns bucket 0 for unparseable or undersize packets. */
static int fq_codel_flow_hash(const void *pkt, int len)
{
    const uint8_t *buf = (const uint8_t *)pkt;
    uint8_t tuple[13];  /* 4 + 4 + 1 + 2 + 2 = 13 bytes for 5-tuple */
    int ti = 0;

    /* Need at least Ethernet header (14) + IP header (20) = 34 bytes */
    if (len < 34)
        return 0;

    const uint8_t *ip = buf + 14;
    int ip_hdr_len;

    /* Check for 802.1Q VLAN tag (EtherType 0x8100) */
    uint16_t ethertype = (uint16_t)((buf[12] << 8) | buf[13]);
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

        /* src IP (4 bytes) */
        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[12 + i];
        /* dst IP (4 bytes) */
        for (int i = 0; i < 4; i++)
            tuple[ti++] = ip[16 + i];
        /* protocol (1 byte) */
        tuple[ti++] = proto;

        /* src/dst ports for TCP/UDP */
        if ((proto == 6 || proto == 17) &&
            (len >= (int)((ip - buf) + ip_hdr_len + 4))) {
            const uint8_t *l4 = ip + ip_hdr_len;
            tuple[ti++] = l4[0]; tuple[ti++] = l4[1]; /* src port */
            tuple[ti++] = l4[2]; tuple[ti++] = l4[3]; /* dst port */
        } else {
            tuple[ti++] = 0; tuple[ti++] = 0;
            tuple[ti++] = 0; tuple[ti++] = 0;
        }
    } else if (ip_version == 6) {
        if (len < (int)((ip - buf) + 40))
            return 0;
        uint8_t proto = ip[6];

        /* src IP (16 bytes) — hash first 4 + last 4 */
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
        /* Unknown L3 — fall back to pointer-based hash */
        return ((uintptr_t)pkt >> 4) % FQ_CODEL_FLOWS;
    }

    uint32_t h = jenkins_hash(tuple, ti);
    return (int)(h % FQ_CODEL_FLOWS);
}

/* ── CoDel control law ────────────────────────────────────────────── */

/* Compute the next drop time using the CoDel control law:
 *   next_drop = now + interval / sqrt(count)
 *
 * Uses a lookup table for 1/sqrt(count) for small counts (the common
 * case), falling back to a rational approximation for larger counts.
 * Returns the absolute tick value for drop_next. */
static uint64_t codel_control_law(uint64_t now, int count,
                                   uint64_t interval_ticks)
{
    uint32_t inv_sqrt;

    /* 1/sqrt(count) in Q10 fixed-point (1024 = 1.0) */
    switch (count) {
    case 0:  inv_sqrt = 1024; break;  /* 1.0 */
    case 1:  inv_sqrt = 1024; break;
    case 2:  inv_sqrt = 724;  break;  /* 1/√2 ≈ 0.707 */
    case 3:  inv_sqrt = 591;  break;  /* 1/√3 ≈ 0.577 */
    case 4:  inv_sqrt = 512;  break;  /* 1/√4 = 0.5 */
    case 5:  inv_sqrt = 458;  break;  /* 1/√5 ≈ 0.447 */
    case 6:  inv_sqrt = 418;  break;
    case 7:  inv_sqrt = 387;  break;
    case 8:  inv_sqrt = 362;  break;
    case 9:  inv_sqrt = 341;  break;
    case 10: inv_sqrt = 324;  break;
    default:
        /* For large counts: approx = 1024 / sqrt(count)
         * Use integer sqrt approximation: sqrt(count) ≈ count / (sqrt(count))
         * Simplified: inv_sqrt = (1024 * 1024) / (count * 32)
         * This avoids floating point entirely. */
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

    /* delay = interval_ticks * inv_sqrt / 1024 */
    uint64_t delay = (interval_ticks * (uint64_t)inv_sqrt) >> 10;
    if (delay < 1)
        delay = 1;
    return now + delay;
}

/* ── CoDel drop decision ─────────────────────────────────────────── */

/* Decide whether to drop (or ECN-mark) the head packet of a flow.
 *
 * CoDel algorithm (per-flow):
 *   1. Compute sojourn time = now - head-packet-enqueue-tick
 *   2. If sojourn < target: clear first_above_time, return 0 (no drop)
 *   3. If sojourn >= target and first_above_time == 0:
 *      record first_above_time = now, return 0
 *   4. If sojourn >= target and (now - first_above_time) >= interval:
 *      enter dropping state, compute next drop time, return 1 (drop)
 *
 * Returns 1 if the head packet should be dropped/marked, 0 otherwise. */
static int fq_codel_should_drop(struct fq_codel_flow *flow, uint64_t now,
                                 uint64_t target_ticks,
                                 uint64_t interval_ticks)
{
    if (flow->count == 0)
        return 0;

    /* Compute sojourn time of the head packet.
     * timer_get_ticks() returns centiseconds (100 Hz → 10 ms/tick).
     * The sojourn in ms = (now - enq_tick) * 10. */
    uint64_t sojourn_ticks = now - flow->enq_tick[flow->head];

    /* Convert ticks to ms for threshold comparisons.
     * TIMER_FREQ = 100 → 1 tick = 10 ms.
     * Target and interval were provided in ticks already. */
    if (sojourn_ticks < target_ticks) {
        /* Below target — not congested */
        flow->first_above_time = 0;
        return 0;
    }

    /* Sojourn exceeds target */
    if (flow->first_above_time == 0) {
        /* First time above target — record the timestamp */
        flow->first_above_time = now;
        return 0;
    }

    /* Check if we've been above target for longer than the interval */
    uint64_t above_ticks = now - flow->first_above_time;
    if (above_ticks >= interval_ticks) {
        /* Time to drop.  Compute next drop time using the control law. */
        flow->drop_next = codel_control_law(now, flow->dropped + 1,
                                             interval_ticks);
        return 1;
    }

    return 0;
}

/* ── ECN marking ──────────────────────────────────────────────────── */

/* Mark a packet with ECN Congestion Experienced (CE) codepoint.
 * Returns 1 if marked, 0 if the packet is not ECN-capable.
 *
 * IPv4: ECN field in TOS byte (byte 1 of IP header), bits 6-7.
 *       CE = 0x03.  ECN-capable transports set ECT(0) = 0x02 or
 *       ECT(1) = 0x01.
 * IPv6: Traffic Class byte, bits 6-7 (same layout).
 */
static int fq_codel_ecn_mark(void *pkt, int len)
{
    uint8_t *buf = (uint8_t *)pkt;
    uint8_t *ip;
    uint16_t ethertype;

    /* Need at least Ethernet header (14 bytes) */
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

    /* Check ECN field (bits 6-7 of TOS/traffic-class byte).
     * 00 = Not ECN-Capable Transport (Non-ECT)
     * 01 = ECT(1)
     * 10 = ECT(0)
     * 11 = Congestion Experienced (CE) */
    uint8_t ecn = ip[1] & 0x03;
    if (ecn == 0x00 || ecn == 0x03)
        return 0; /* Not ECN-capable, or already CE */

    /* Set CE codepoint */
    ip[1] |= 0x03;
    return 1;
}

/* ── Enqueue ──────────────────────────────────────────────────────── */

/* Enqueue a packet into the FQ-CoDel scheduler.
 *
 * @pkt:      pointer to the raw Ethernet frame
 * @len:      length of the frame in bytes
 * @src_ip:   source IP address (32-bit, for hashing)
 * @dst_ip:   destination IP address
 * @src_port: source transport port
 * @dst_port: destination transport port
 * @proto:    IP protocol number
 *
 * Returns 0 on success, -ENOBUFS if the flow queue is full. */
int fq_codel_enqueue(const void *pkt, size_t len,
                     uint32_t src_ip, uint32_t dst_ip,
                     uint16_t src_port, uint16_t dst_port,
                     uint8_t proto)
{
    (void)src_ip;
    (void)dst_ip;
    (void)src_port;
    (void)dst_port;
    (void)proto;

    /* Classify into a flow bucket via 5-tuple hash from the raw frame.
     * We hash the actual packet contents for accuracy. */
    int bucket = fq_codel_flow_hash(pkt, (int)len);
    struct fq_codel_flow *flow = &fq_codel.flows[bucket];

    /* Check per-flow queue limit */
    if (flow->count >= fq_codel.limit) {
        fq_codel.total_dropped++;
        return -ENOBUFS;
    }

    /* Store the packet pointer and metadata */
    flow->queue[flow->tail] = (void *)pkt;
    flow->pkt_len[flow->tail] = (int)len;
    flow->enq_tick[flow->tail] = timer_get_ticks();
    flow->tail = (flow->tail + 1) % FQ_CODEL_LIMIT;
    flow->count++;

    return 0;
}

/* ── Dequeue with CoDel AQM ──────────────────────────────────────── */

/* Dequeue a packet from the FQ-CoDel scheduler.
 *
 * Implements:
 *   1. Deficit Round Robin (DRR) scan across all flow buckets,
 *      starting from the last dequeue position (flow_offset).
 *   2. For each non-empty flow, apply the CoDel drop/mark decision
 *      to the head packet before dequeuing.
 *   3. If CoDel decides to drop, advance past the head packet and
 *      re-check the new head (or skip to the next flow).
 *   4. Return the first packet that survives CoDel.
 *
 * @pkt:     output buffer for the dequeued packet
 * @max_len: in/out — max capacity on entry, actual data length on return
 *
 * Returns the number of bytes written to @pkt, or 0 if no packet. */
int fq_codel_dequeue(void *pkt, size_t *max_len)
{
    if (!pkt || !max_len || *max_len == 0)
        return 0;

    uint64_t now = timer_get_ticks();

    /* Convert ms thresholds to ticks.
     * TIMER_FREQ = 100 Hz → 1 tick = 10 ms.
     * target_ms / 10 → target_ticks */
    uint64_t target_ticks   = FQ_CODEL_TARGET_MS / 10;
    uint64_t interval_ticks = FQ_CODEL_INTERVAL_MS / 10;
    if (target_ticks < 1)
        target_ticks = 1;
    if (interval_ticks < 1)
        interval_ticks = 1;

    int start = fq_codel.flow_offset;

    for (int i = 0; i < FQ_CODEL_FLOWS; i++) {
        int b = (start + i) % FQ_CODEL_FLOWS;
        struct fq_codel_flow *flow = &fq_codel.flows[b];

        if (flow->count == 0) {
            flow->deficit = 0;  /* reset deficit for idle flows */
            continue;
        }

        /* DRR: refresh deficit */
        if (flow->deficit <= 0)
            flow->deficit += flow->quantum;

        int head_len = flow->pkt_len[flow->head];

        /* DRR: skip if head packet exceeds deficit */
        if (head_len > flow->deficit)
            continue;

        /* ── CoDel AQM on the head packet ──────────────────────── */

        /* Check whether to drop or mark the current head */
        if (fq_codel_should_drop(flow, now, target_ticks, interval_ticks)) {
            /* Try ECN marking first if enabled */
            if (fq_codel.ecn_enabled &&
                fq_codel_ecn_mark(flow->queue[flow->head],
                                  flow->pkt_len[flow->head])) {
                /* ECN-marked — advance past the marked packet (keep it).
                 * The packet is still dequeued below with CE set. */
                flow->ecn_marked++;
                fq_codel.total_ecn_marked++;
            } else {
                /* Drop the head packet and skip to next flow */
                flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
                flow->count--;
                flow->dropped++;
                fq_codel.total_dropped++;
                /* Re-check the new head (if any) for dropping */
                if (flow->count == 0)
                    continue;
                if (fq_codel_should_drop(flow, now, target_ticks,
                                          interval_ticks)) {
                    /* Still congested — drop this one too */
                    flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
                    flow->count--;
                    flow->dropped++;
                    fq_codel.total_dropped++;
                    continue;
                }
                /* New head is OK — update head_len for DRR */
                head_len = flow->pkt_len[flow->head];
            }
        }

        /* DRR: deduct from deficit */
        flow->deficit -= head_len;

        /* Dequeue the head packet (copy to caller's buffer) */
        size_t copy_len = (size_t)head_len;
        if (copy_len > *max_len)
            copy_len = *max_len;

        memcpy(pkt, flow->queue[flow->head], copy_len);
        flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
        flow->count--;
        fq_codel.total_dequeued++;

        /* Advance round-robin pointer for next dequeue */
        fq_codel.flow_offset = (b + 1) % FQ_CODEL_FLOWS;

        *max_len = copy_len;
        return (int)copy_len;
    }

    return 0;  /* no packet available */
}

/* ── Drop from longest queue ──────────────────────────────────────── */

/* Drop one packet from the flow with the most queued packets (fair drop).
 * Returns 0 on success, -ENOENT if no packets to drop. */
int fq_codel_drop(void)
{
    int longest = 0;
    int longest_idx = -1;

    for (int i = 0; i < FQ_CODEL_FLOWS; i++) {
        if (fq_codel.flows[i].count > longest) {
            longest = fq_codel.flows[i].count;
            longest_idx = i;
        }
    }

    if (longest_idx < 0 || longest == 0)
        return -ENOENT;

    struct fq_codel_flow *flow = &fq_codel.flows[longest_idx];
    flow->head = (flow->head + 1) % FQ_CODEL_LIMIT;
    flow->count--;
    flow->dropped++;
    fq_codel.total_dropped++;
    return 0;
}

/* ── Queue length query ────────────────────────────────────────────── */

/* Return the total number of queued packets across all flows. */
int fq_codel_qlen(void)
{
    int total = 0;
    for (int i = 0; i < FQ_CODEL_FLOWS; i++)
        total += fq_codel.flows[i].count;
    return total;
}

/* Return the number of flows that currently have at least one packet. */
int fq_codel_flow_count(void)
{
    int count = 0;
    for (int i = 0; i < FQ_CODEL_FLOWS; i++) {
        if (fq_codel.flows[i].count > 0)
            count++;
    }
    return count;
}

/* ── Statistics ────────────────────────────────────────────────────── */

/* Collect aggregate statistics.  Any parameter may be NULL to skip. */
void fq_codel_get_stats(uint64_t *dropped, uint64_t *ecn_marked,
                         uint64_t *dequeued, int *active_flows)
{
    if (dropped)
        *dropped = fq_codel.total_dropped;
    if (ecn_marked)
        *ecn_marked = fq_codel.total_ecn_marked;
    if (dequeued)
        *dequeued = fq_codel.total_dequeued;
    if (active_flows)
        *active_flows = fq_codel_flow_count();
}

/* ── Reset ────────────────────────────────────────────────────────── */

/* Reset the FQ-CoDel scheduler, clearing all flow queues and state.
 * This is the equivalent of qdisc reset — drops all queued packets
 * and reinitialises CoDel state without freeing resources. */
int fq_codel_reset(void)
{
    for (int i = 0; i < FQ_CODEL_FLOWS; i++) {
        struct fq_codel_flow *flow = &fq_codel.flows[i];
        flow->head = 0;
        flow->tail = 0;
        flow->count = 0;
        flow->deficit = 0;
        flow->first_above_time = 0;
        flow->dropping = 0;
        flow->drop_next = 0;
    }
    fq_codel.flow_offset = 0;
    return 0;
}

/* ── Destroy ──────────────────────────────────────────────────────── */

/* Tear down the FQ-CoDel scheduler.  For this static instance,
 * this is equivalent to reset (the memory is not freed since it
 * is statically allocated).  Returns 0 on success. */
int fq_codel_destroy(void)
{
    return fq_codel_reset();
}

/* ── Configuration change ─────────────────────────────────────────── */

/* Update FQ-CoDel configuration parameters.
 *
 * @cfg: pointer to a configuration structure
 * @cfg_len: size of the configuration data
 *
 * Accepted structures:
 *   - uint32_t[1]: new quantum value
 *   - uint32_t[2]: { quantum, limit }
 *   - uint32_t[3]: { quantum, limit, ecn_enabled }
 *
 * Returns 0 on success, -EINVAL on invalid input. */
int fq_codel_change(const void *cfg, size_t cfg_len)
{
    if (!cfg || cfg_len < sizeof(uint32_t))
        return -EINVAL;

    const uint32_t *params = (const uint32_t *)cfg;

    /* First parameter is always the quantum */
    if (params[0] > 0)
        fq_codel.quantum = (int)params[0];

    if (cfg_len >= sizeof(uint32_t) * 2) {
        if (params[1] > 0 && params[1] <= FQ_CODEL_LIMIT)
            fq_codel.limit = (int)params[1];
    }

    if (cfg_len >= sizeof(uint32_t) * 3)
        fq_codel.ecn_enabled = params[2] ? 1 : 0;

    /* Propagate quantum to all existing flows */
    for (int i = 0; i < FQ_CODEL_FLOWS; i++)
        fq_codel.flows[i].quantum = fq_codel.quantum;

    return 0;
}

/* ── Dump statistics ──────────────────────────────────────────────── */

/* Fill a text buffer with human-readable FQ-CoDel statistics.
 *
 * @skb: pointer to a stats buffer (text)
 * @skb_len: maximum bytes to write
 *
 * Returns the number of bytes written, or 0 on error. */
int fq_codel_dump(char *skb, size_t skb_len)
{
    if (!skb || skb_len == 0)
        return 0;

    int active = fq_codel_flow_count();
    int total_qlen = fq_codel_qlen();

    int n = snprintf(skb, skb_len,
        "fq_codel:\n"
        "  flows:     %d active / %d total\n"
        "  quantum:   %d bytes\n"
        "  limit:     %d packets/flow\n"
        "  ecn:       %s\n"
        "  dropped:   %llu\n"
        "  ecn_marked: %llu\n"
        "  dequeued:  %llu\n"
        "  qlen:      %d packets\n",
        active, FQ_CODEL_FLOWS,
        fq_codel.quantum,
        fq_codel.limit,
        fq_codel.ecn_enabled ? "enabled" : "disabled",
        (unsigned long long)fq_codel.total_dropped,
        (unsigned long long)fq_codel.total_ecn_marked,
        (unsigned long long)fq_codel.total_dequeued,
        total_qlen);

    return n;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void fq_codel_init(void)
{
    memset(&fq_codel, 0, sizeof(fq_codel));
    fq_codel.quantum = FQ_CODEL_QUANTUM;
    fq_codel.limit = FQ_CODEL_LIMIT;
    fq_codel.ecn_enabled = FQ_CODEL_DEFAULT_ECN;

    /* Initialise per-flow quantum */
    for (int i = 0; i < FQ_CODEL_FLOWS; i++)
        fq_codel.flows[i].quantum = FQ_CODEL_QUANTUM;

    kprintf("[OK] FQ-CoDel — Fair Queuing with Controlled Delay\n");
}

#include "module.h"
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
MODULE_DESCRIPTION("FQ-CoDel: Fair Queuing with Controlled Delay AQM");
MODULE_AUTHOR("OS Kernel Team");
module_init(fq_codel_init);
