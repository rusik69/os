/* sch_tbf.c — Token Bucket Filter (TBF) qdisc
 *
 * A simple classless shaper that limits traffic to a configured rate
 * using a token bucket.  Packets are queued in a FIFO and only
 * dequeued when sufficient tokens are available.
 *
 * Token refill: every timer tick, (rate / TIMER_FREQ) bytes worth of
 * tokens are added, capped at burst.  Tokens use fixed-point with
 * 8 fractional bits (matching the HTB convention in pkt_sched.c).
 *
 * Rate limit: the sustained rate in bytes per second.
 * Burst:      the maximum number of bytes that can accumulate in the
 *             bucket (allows short bursts above the sustained rate).
 * Limit:      the maximum number of packets the FIFO queue can hold.
 * MTU:        the minimum packet unit for token charging — no packet
 *             consumes fewer than mtu bytes worth of tokens.
 */

#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"

/* ── Constants ────────────────────────────────────────────────── */

#define TBF_DEFAULT_RATE     1000000   /* 1 Mbps default */
#define TBF_DEFAULT_BURST    0         /* 0 = auto-compute */
#define TBF_DEFAULT_LIMIT    128       /* max 128 packets in queue */
#define TBF_DEFAULT_MTU      64        /* minimum packet unit */
#define TBF_QUEUE_LIMIT      1024      /* hard cap on queue depth */
#define TBF_MAX_BURST        256000    /* 256 KB max burst sanity cap */
#define TBF_MIN_BURST        1500      /* at least 1 MTU */

/* Auto-compute a reasonable default burst from the rate.
 * Formula: rate / 5 (200ms worth of data at rate) clamped to
 * [TBF_MIN_BURST, TBF_MAX_BURST]. */
static uint32_t tbf_auto_burst(uint32_t rate)
{
    uint32_t b = rate / 5;
    if (b < TBF_MIN_BURST)
        b = TBF_MIN_BURST;
    if (b > TBF_MAX_BURST)
        b = TBF_MAX_BURST;
    return b;
}

/* ── TBF private state ────────────────────────────────────────── */

struct tbf_priv {
    /* Configuration */
    uint32_t rate;         /* sustained rate (bytes per second) */
    uint32_t burst;        /* max token accumulation (bytes) */
    uint32_t limit;        /* max queue depth (packets) */
    uint32_t mtu;          /* minimum packet unit */

    /* Token bucket — fixed-point with 8 fractional bits */
    int64_t  tokens;       /* current token balance (bytes << 8) */
    uint64_t last_touched; /* timer tick of last token refill */

    /* Packet FIFO queue */
    void    *queue[TBF_QUEUE_LIMIT];
    int      pkt_len[TBF_QUEUE_LIMIT];
    int      head;
    int      tail;
    int      count;

    /* Statistics */
    uint64_t drops;        /* packets dropped due to queue full */
    uint64_t throttles;    /* packets delayed/throttled (no tokens) */
    uint64_t dequeues;     /* packets successfully dequeued */
};

/* ── Token refill ─────────────────────────────────────────────── */

/* Refill tokens based on elapsed time since last_touched.
 * Tokens accumulate at rate bytes/second up to burst. */
static void tbf_refill(struct tbf_priv *tp, uint64_t now)
{
    if (now <= tp->last_touched)
        return;

    uint64_t elapsed = now - tp->last_touched;
    /* TIMER_FREQ = 100 ticks/second (from timer.h) */
    int64_t add = (int64_t)(elapsed * (uint64_t)tp->rate) / TIMER_FREQ;
    tp->tokens += add << 8;

    int64_t max_tokens = (int64_t)tp->burst << 8;
    if (tp->tokens > max_tokens)
        tp->tokens = max_tokens;

    tp->last_touched = now;
}

/* ── Charged length ───────────────────────────────────────────── */

/* Compute the token charge length for a packet.  No packet is
 * charged less than mtu bytes — this prevents tiny ACKs from
 * bypassing the rate limit. */
static inline int tbf_charged_len(struct tbf_priv *tp, int len)
{
    return (len < (int)tp->mtu) ? (int)tp->mtu : len;
}

/* ── Enqueue ──────────────────────────────────────────────────── */

static int tbf_enqueue(struct qdisc *q, void *pkt, int len)
{
    struct tbf_priv *tp = (struct tbf_priv *)q->priv;
    if (!tp)
        return -1;

    if (tp->count >= (int)tp->limit) {
        tp->drops++;
        return -1;  /* queue full */
    }

    if (tp->count >= TBF_QUEUE_LIMIT) {
        tp->drops++;
        return -1;  /* hard cap reached */
    }

    tp->queue[tp->tail] = pkt;
    tp->pkt_len[tp->tail] = len;
    tp->tail = (tp->tail + 1) % TBF_QUEUE_LIMIT;
    tp->count++;
    return 0;
}

/* ── Dequeue ──────────────────────────────────────────────────── */

static void *tbf_dequeue(struct qdisc *q)
{
    struct tbf_priv *tp = (struct tbf_priv *)q->priv;
    if (!tp || tp->count == 0)
        return NULL;

    uint64_t now = timer_get_ticks();

    /* Refill tokens first */
    tbf_refill(tp, now);

    /* Check if the head packet fits in the token bucket */
    int head_len = tp->pkt_len[tp->head];
    int charge = tbf_charged_len(tp, head_len);

    if (tp->tokens < ((int64_t)charge << 8)) {
        /* Not enough tokens — throttle */
        tp->throttles++;
        return NULL;
    }

    /* Consume tokens */
    tp->tokens -= (int64_t)charge << 8;

    /* Dequeue the packet */
    void *pkt = tp->queue[tp->head];
    tp->head = (tp->head + 1) % TBF_QUEUE_LIMIT;
    tp->count--;
    tp->dequeues++;

    return pkt;
}

/* ── Drop ──────────────────────────────────────────────────────── */

/* Drop the packet at the tail of the queue (tail-drop). */
static int tbf_drop(struct qdisc *q)
{
    struct tbf_priv *tp = (struct tbf_priv *)q->priv;
    if (!tp || tp->count == 0)
        return -1;

    tp->tail = (tp->tail - 1 + TBF_QUEUE_LIMIT) % TBF_QUEUE_LIMIT;
    tp->count--;
    tp->drops++;
    return 0;
}

/* ── Create ────────────────────────────────────────────────────── */

struct qdisc *tbf_create(const struct tbf_spec *spec)
{
    struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
    if (!q)
        return NULL;

    struct tbf_priv *tp = (struct tbf_priv *)kmalloc(sizeof(struct tbf_priv));
    if (!tp) {
        kfree(q);
        return NULL;
    }

    memset(tp, 0, sizeof(*tp));

    if (spec) {
        tp->rate  = spec->rate ? spec->rate : TBF_DEFAULT_RATE;
        tp->burst = spec->burst ? spec->burst : tbf_auto_burst(tp->rate);
        tp->limit = spec->limit ? spec->limit : TBF_DEFAULT_LIMIT;
        tp->mtu   = spec->mtu   ? spec->mtu   : TBF_DEFAULT_MTU;
    } else {
        tp->rate  = TBF_DEFAULT_RATE;
        tp->burst = tbf_auto_burst(tp->rate);
        tp->limit = TBF_DEFAULT_LIMIT;
        tp->mtu   = TBF_DEFAULT_MTU;
    }

    /* Validate burst */
    if (tp->burst < TBF_MIN_BURST)
        tp->burst = TBF_MIN_BURST;
    if (tp->burst > TBF_MAX_BURST)
        tp->burst = TBF_MAX_BURST;

    /* Pre-fill tokens to allow initial burst */
    tp->tokens = (int64_t)tp->burst << 8;
    tp->last_touched = timer_get_ticks();

    q->type    = QDISC_TBF;
    q->priv    = tp;
    q->enqueue = tbf_enqueue;
    q->dequeue = tbf_dequeue;
    q->drop    = tbf_drop;

    return q;
}

/* ── Module registration ──────────────────────────────────────── */

#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Token Bucket Filter (TBF) qdisc — rate + burst shaping");
MODULE_AUTHOR("1000 Changes Project");
