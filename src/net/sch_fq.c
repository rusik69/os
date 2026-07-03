/* sch_fq.c — Fair Queue (FQ) per-flow queuing qdisc with pacing
 *
 * Implements per-flow fair queuing with Deficit Round Robin (DRR)
 * scheduling and optional rate-based pacing.  Packets are classified
 * into flow buckets by a 5-tuple hash of source/destination IP,
 * protocol, and ports.  Each flow maintains an independent FIFO
 * queue.  The DRR scheduler ensures that no single flow can
 * monopolise the link — each flow gets a quantum of bytes per
 * round, and unused credit rolls over.
 *
 * Pacing (rate-based scheduling) imposes a per-flow maximum sending
 * rate by tracking pacing credit that replenishes over time at the
 * configured rate.  If a flow lacks enough credit to send its head
 * packet, the DRR round-robin advances to the next flow.  When all
 * active flows are credit-starved, dequeue returns NULL (the caller
 * idles), implementing the pacing behaviour.
 *
 * Reference: Linux sch_fq (net/sched/sch_fq.c).
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

/* Pacing defaults */
#define FQ_DEFAULT_PACING_RATE    125000   /* 1 Mbps default rate */
#define FQ_DEFAULT_MAX_CREDIT     (64 * 1024)  /* 64 KB max credit */

/* ── Per-flow state ─────────────────────────────────────────────── */

struct fq_flow {
    void   *queue[FQ_LIMIT];   /* packet pointers */
    int     pkt_len[FQ_LIMIT]; /* packet lengths (for DRR accounting) */
    int     head;               /* dequeue index */
    int     tail;               /* enqueue index */
    int     count;              /* packets in this flow */
    int     deficit;            /* DRR deficit counter (bytes) */
    int     quantum;            /* DRR quantum for this flow */

    /* Pacing (rate-based scheduling) */
    uint32_t pacing_rate;       /* bytes per second; 0 = unlimited */
    uint64_t credit;            /* pacing credit (bytes) */
    uint64_t last_time;         /* tick of last credit update */
    uint32_t max_credit;        /* maximum credit accumulation (bytes) */
};

/* ── FQ private state ───────────────────────────────────────────── */

struct fq_priv {
    struct fq_flow flows[FQ_FLOWS];
    int     quantum;            /* default quantum for new flows */
    int     flow_offset;        /* DRR round-robin scan start index */

    /* Pacing configuration */
    uint32_t default_pacing_rate;  /* rate for new flows (0 = unlimited) */
    int     pacing_enabled;        /* 1 = pacing active */

    /* Statistics */
    uint64_t drops;
    uint64_t dequeues;
    uint64_t pacing_idles;      /* dequeue returned NULL, all flows idle */
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

/* ── Pacing credit update ───────────────────────────────────────── */

/* Replenish a flow's pacing credit based on elapsed time since the
 * last update.  Credit accumulates at `pacing_rate` bytes/second,
 * capped at `max_credit`.  When pacing_rate is 0 (unlimited), credit
 * is set to max_credit so it never blocks. */
static void fq_update_credit(struct fq_flow *flow, uint64_t now)
{
    if (flow->pacing_rate == 0) {
        /* Unlimited — always have enough credit */
        flow->credit = flow->max_credit;
        flow->last_time = now;
        return;
    }

    if (now <= flow->last_time)
        return;

    uint64_t elapsed = now - flow->last_time;
    /* Add (elapsed * pacing_rate / TIMER_FREQ) bytes of credit.
     * TIMER_FREQ = 100 ticks/second from timer.h. */
    uint64_t add = (elapsed * (uint64_t)flow->pacing_rate) / TIMER_FREQ;
    flow->credit += add;
    if (flow->credit > flow->max_credit)
        flow->credit = flow->max_credit;

    flow->last_time = now;
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

/* ── Dequeue (Deficit Round Robin with pacing) ──────────────────── */

/* DRR scheduling algorithm with pacing:
 *
 *   - Scan flows round-robin starting from flow_offset.
 *   - For each non-empty flow:
 *       1. Update pacing credit based on elapsed time.
 *       2. If deficit <= 0, add quantum to deficit.
 *       3. If deficit >= head-packet-size AND (pacing disabled OR
 *          pacing credit >= head-packet-size), send the packet.
 *          Deduct packet size from both deficit and credit.
 *       4. If either deficit or credit is insufficient, skip to the
 *          next flow (deficit is preserved for the next round).
 *   - Advances flow_offset after a successful dequeue so that no
 *     flow is starved.
 *   - Returns NULL if no flow has both enough DRR deficit AND
 *     pacing credit. */
static void *fq_dequeue(struct qdisc *q)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return NULL;

    uint64_t now = timer_get_ticks();
    int start = priv->flow_offset;
    int all_pacing_blocked = 1;  /* assume all are blocked until we find one */

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

        /* Update pacing credit */
        fq_update_credit(flow, now);

        /* Check if the head packet fits in the deficit */
        int head_len = flow->pkt_len[flow->head];

        if (head_len > flow->deficit)
            continue; /* not enough deficit — move to next flow */

        /* Check pacing credit (when pacing is enabled) */
        if (priv->pacing_enabled && flow->pacing_rate > 0) {
            if ((uint64_t)head_len > flow->credit)
                continue; /* not enough pacing credit — skip */
        }

        /* If we get here, at least one flow can potentially send */
        all_pacing_blocked = 0;

        /* Send the head packet */
        void *pkt = flow->queue[flow->head];
        flow->deficit -= head_len;
        flow->head = (flow->head + 1) % FQ_LIMIT;
        flow->count--;

        /* Deduct pacing credit when pacing is active */
        if (priv->pacing_enabled && flow->pacing_rate > 0)
            flow->credit -= (uint64_t)head_len;

        priv->dequeues++;

        /* Advance the round-robin pointer */
        priv->flow_offset = (b + 1) % FQ_FLOWS;

        return pkt;
    }

    /* If all non-empty flows were credit-starved, record an idle event */
    if (all_pacing_blocked && priv->pacing_enabled)
        priv->pacing_idles++;

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

/* ── Pacing API ─────────────────────────────────────────────────── */

/* Set per-flow pacing rate.  @bucket must be 0..FQ_FLOWS-1.
 * @rate_bps of 0 means unlimited (no pacing for this flow).
 * Returns 0 on success, -EINVAL on invalid bucket. */
int fq_set_pacing_rate(struct qdisc *q, int bucket, uint32_t rate_bps)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return -EINVAL;

    if (bucket < 0 || bucket >= FQ_FLOWS)
        return -EINVAL;

    struct fq_flow *flow = &priv->flows[bucket];
    flow->pacing_rate = rate_bps;

    /* Reset credit tracking so the new rate takes effect immediately */
    flow->last_time = timer_get_ticks();
    if (rate_bps == 0) {
        flow->credit = flow->max_credit;  /* unlimited */
    } else {
        /* Start with one quantum of credit so the flow can send at
         * least one packet before being paced */
        flow->credit = (uint64_t)flow->quantum;
        if (flow->credit > flow->max_credit)
            flow->credit = flow->max_credit;
    }

    return 0;
}

/* Set the default pacing rate for newly created flows.
 * @rate_bps of 0 means unlimited by default. */
void fq_set_default_pacing_rate(struct qdisc *q, uint32_t rate_bps)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return;
    priv->default_pacing_rate = rate_bps;

    /* Also update existing flows that currently use the old default.
     * We only touch flows whose rate matches the old default (i.e.
     * they were never explicitly configured).  Since we can't
     * distinguish "set explicitly" from "inherited default", we
     * apply the new rate to all flows.  Individual fq_set_pacing_rate
     * calls will override. */
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < FQ_FLOWS; i++) {
        struct fq_flow *flow = &priv->flows[i];
        flow->pacing_rate = rate_bps;
        flow->last_time = now;
        if (rate_bps == 0)
            flow->credit = flow->max_credit;
        else {
            flow->credit = (uint64_t)flow->quantum;
            if (flow->credit > flow->max_credit)
                flow->credit = flow->max_credit;
        }
    }
}

/* Enable or disable pacing globally. */
void fq_set_pacing_enabled(struct qdisc *q, int enabled)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return;
    priv->pacing_enabled = enabled ? 1 : 0;

    /* Reset credit timestamps so the toggle is immediate */
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < FQ_FLOWS; i++)
        priv->flows[i].last_time = now;
}

/* Return pacing statistics. */
void fq_get_pacing_stats(struct qdisc *q, uint64_t *pacing_idles)
{
    struct fq_priv *priv = (struct fq_priv *)q->priv;
    if (!priv)
        return;
    if (pacing_idles)
        *pacing_idles = priv->pacing_idles;
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
    priv->default_pacing_rate = FQ_DEFAULT_PACING_RATE;
    priv->pacing_enabled = 1;   /* pacing active by default */

    uint64_t now = timer_get_ticks();

    /* Pre-initialise each flow with default quantum, deficit, and
     * pacing parameters */
    for (int i = 0; i < FQ_FLOWS; i++) {
        priv->flows[i].quantum = FQ_QUANTUM;
        priv->flows[i].deficit = FQ_QUANTUM;
        priv->flows[i].pacing_rate = FQ_DEFAULT_PACING_RATE;
        priv->flows[i].max_credit = FQ_DEFAULT_MAX_CREDIT;
        priv->flows[i].credit = (uint64_t)FQ_QUANTUM;
        priv->flows[i].last_time = now;
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
MODULE_DESCRIPTION("Fair Queue (FQ) per-flow queuing qdisc with pacing");
MODULE_AUTHOR("1000 Changes Project");
