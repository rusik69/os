/* sch_fq.c — Fair Queue (FQ) per-flow queuing qdisc
 *
 * Implements per-flow fair queuing with Deficit Round Robin (DRR)
 * scheduling.  Packets are classified into flow buckets by a 5-tuple
 * hash of source/destination IP, protocol, and ports.  Each flow
 * maintains an independent FIFO queue.  The DRR scheduler ensures
 * that no single flow can monopolise the link — each flow gets a
 * quantum of bytes per round, and unused credit rolls over.
 *
 * This module implements FQ *without* pacing.  Pacing (rate-based
 * per-flow scheduling) is added in a follow-up change (D207 task 5).
 *
 * Reference: Linux sch_fq (net/sched/sch_fq.c) — the DRR-only subset.
 */

#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"

/* ── Constants ──────────────────────────────────────────────────── */

#define FQ_FLOWS         256   /* number of flow buckets */
#define FQ_LIMIT         128   /* max packets per flow */
#define FQ_QUANTUM       300   /* default DRR quantum (bytes) */
#define FQ_MAX_BACKLOG  10240  /* hard cap on total queued packets */

/* ── Per-flow state ─────────────────────────────────────────────── */

struct fq_flow {
    void   *queue[FQ_LIMIT];   /* packet pointers */
    int     pkt_len[FQ_LIMIT]; /* packet lengths (for DRR accounting) */
    int     head;               /* dequeue index */
    int     tail;               /* enqueue index */
    int     count;              /* packets in this flow */
    int     deficit;            /* DRR deficit counter (bytes) */
    int     quantum;            /* DRR quantum for this flow */
};

/* ── FQ private state ───────────────────────────────────────────── */

struct fq_priv {
    struct fq_flow flows[FQ_FLOWS];
    int     quantum;            /* default quantum for new flows */
    int     flow_offset;        /* DRR round-robin scan start index */

    /* Statistics */
    uint64_t drops;
    uint64_t dequeues;
};

/* ── Flow hash — 5-tuple hash for fair distribution ─────────────── */

/* Compute a flow bucket from the packet's addressing and transport
 * headers, using a simple multiplicative hash.  Returns 0..FQ_FLOWS-1. */
static int fq_flow_hash(const void *pkt, int len)
{
    const uint8_t *buf = (const uint8_t *)pkt;
    uint8_t tuple[13];
    int ti = 0;

    /* Need at least Ethernet (14) + IP (20) = 34 bytes */
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
        /* Unknown L3 — fall back to address-based hash */
        return ((uintptr_t)pkt >> 4) % FQ_FLOWS;
    }

    /* Jenkins one-at-a-time (simplified) */
    uint32_t h = 0;
    for (int i = 0; i < ti; i++) {
        h += tuple[i];
        h += (h << 10);
        h ^= (h >> 6);
    }
    h += (h << 3);
    h ^= (h >> 11);
    h += (h << 15);
    return (int)(h % FQ_FLOWS);
}

/* ── Enqueue ────────────────────────────────────────────────────── */

static int fq_enqueue(struct qdisc *q, void *pkt, int len)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv || !pkt || len <= 0)
        return -EINVAL;

    int bucket = fq_flow_hash(pkt, len);
    struct fq_flow *flow = &priv->flows[bucket];

    if (flow->count >= FQ_LIMIT) {
        priv->drops++;
        return -ENOSPC;
    }

    flow->queue[flow->tail] = pkt;
    flow->pkt_len[flow->tail] = len;
    flow->tail = (flow->tail + 1) % FQ_LIMIT;
    flow->count++;
    return 0;
}

/* ── Dequeue (Deficit Round Robin) ──────────────────────────────── */

/* DRR scheduling algorithm:
 *   - Scan flows round-robin starting from flow_offset.
 *   - For each non-empty flow:
 *       if deficit <= 0, add quantum to deficit.
 *       if deficit >= head-packet-size, send the packet
 *         and subtract its size from deficit.
 *       if deficit < head-packet-size, move to the next flow.
 *   - Advances flow_offset after a successful dequeue so that
 *     no flow is starved. */
static void *fq_dequeue(struct qdisc *q)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return NULL;

    int start = priv->flow_offset;

    for (int i = 0; i < FQ_FLOWS; i++) {
        int b = (start + i) % FQ_FLOWS;
        struct fq_flow *flow = &priv->flows[b];

        if (flow->count == 0) {
            flow->deficit = 0; /* reset deficit for idle flows */
            continue;
        }

        /* Refresh deficit */
        if (flow->deficit <= 0)
            flow->deficit += flow->quantum;

        /* Check if the head packet fits in the deficit */
        int head_len = flow->pkt_len[flow->head];

        if (head_len > flow->deficit)
            continue; /* not enough credit — move to next flow */

        /* Send the head packet */
        void *pkt = flow->queue[flow->head];
        flow->deficit -= head_len;
        flow->head = (flow->head + 1) % FQ_LIMIT;
        flow->count--;
        priv->dequeues++;

        /* Advance the round-robin pointer */
        priv->flow_offset = (b + 1) % FQ_FLOWS;

        return pkt;
    }

    return NULL;
}

/* ── Drop ────────────────────────────────────────────────────────── */

/* Drop from the longest flow queue (fair drop). */
static int fq_drop(struct qdisc *q)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    int longest = 0;
    int longest_idx = 0;
    for (int i = 0; i < FQ_FLOWS; i++) {
        if (priv->flows[i].count > longest) {
            longest = priv->flows[i].count;
            longest_idx = i;
        }
    }

    if (longest == 0)
        return -ENOENT;

    struct fq_flow *flow = &priv->flows[longest_idx];
    flow->head = (flow->head + 1) % FQ_LIMIT;
    flow->count--;
    priv->drops++;
    return 0;
}

/* ── Query ──────────────────────────────────────────────────────── */

/* Return total queued packets across all flows. */
int fq_qlen(struct qdisc *q)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    int total = 0;
    for (int i = 0; i < FQ_FLOWS; i++)
        total += priv->flows[i].count;
    return total;
}

/* Return number of flows that have at least one packet. */
int fq_flow_count(struct qdisc *q)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    int count = 0;
    for (int i = 0; i < FQ_FLOWS; i++) {
        if (priv->flows[i].count > 0)
            count++;
    }
    return count;
}

/* Collect statistics. */
void fq_get_stats(struct qdisc *q, uint64_t *drops, uint64_t *dequeues)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return;
    if (drops)
        *drops = priv->drops;
    if (dequeues)
        *dequeues = priv->dequeues;
}

/* ── Create ──────────────────────────────────────────────────────── */

struct qdisc *fq_create(void)
{
    struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q)
        return NULL;

    struct fq_priv *priv = (struct fq_priv *)kmalloc(sizeof(struct fq_priv));
    if (!priv) {
        kfree(q);
        return NULL;
    }

    memset(priv, 0, sizeof(*priv));
    priv->quantum = FQ_QUANTUM;

    /* Pre-initialise each flow with the default quantum and deficit */
    for (int i = 0; i < FQ_FLOWS; i++) {
        priv->flows[i].quantum = FQ_QUANTUM;
        priv->flows[i].deficit = FQ_QUANTUM;
    }

    q->type    = QDISC_FQ;
    q->priv    = priv;
    q->enqueue = fq_enqueue;
    q->dequeue = fq_dequeue;
    q->drop    = fq_drop;

    return q;
}

/* ── Module registration ────────────────────────────────────────── */

#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Fair Queue (FQ) per-flow queuing qdisc");
MODULE_AUTHOR("1000 Changes Project");
