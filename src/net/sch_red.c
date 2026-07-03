/* sch_red.c — Random Early Detection (RED) AQM qdisc
 *
 * Implements the classic RED (Random Early Detection) active queue
 * management algorithm from Floyd & Jacobson (1993).  RED maintains
 * an exponential weighted moving average (EWMA) of the queue length
 * and drops (or ECN-marks) packets probabilistically when the average
 * queue length exceeds a configurable minimum threshold.
 *
 * Key properties:
 *   - EWMA average queue tracking with configurable weight (w_q)
 *   - Three regimes: below min_th → no action (queue grows harmlessly)
 *                    between min_th and max_th → probabilistic drop/mark
 *                    above max_th → forced drop (tail-drop)
 *   - Optional ECN marking (Congestion Experienced codepoint)
 *   - "Gentle" mode: drop probability ramps linearly from max_p to 1
 *     between max_th and 2 * max_th (when enabled)
 *
 * Reference: Floyd & Jacobson, "Random Early Detection Gateways for
 * Congestion Avoidance", IEEE/ACM Transactions on Networking, 1993.
 */

#define KERNEL_INTERNAL
#include "pkt_sched.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"
#include "rng.h"

/* ── Constants ────────────────────────────────────────────────── */

/* Default RED parameters */
#define RED_DEFAULT_MIN_TH     20       /* default min threshold (packets) */
#define RED_DEFAULT_MAX_TH     60       /* default max threshold (packets) */
#define RED_DEFAULT_MAX_P      10       /* default max_p = 1/10 */
#define RED_DEFAULT_WQ_DIV     512      /* default w_q = 1/512 */
#define RED_DEFAULT_LIMIT      256      /* max queue depth (packets) */
#define RED_QUEUE_HARD_LIMIT   1024     /* hard cap on queue */

/* Fixed-point precision */
#define RED_AVG_SHIFT          8        /* fractional bits for average queue */
#define RED_PROB_SHIFT         16       /* fractional bits for probability */

/* Maximum probability value in fixed-point (represents 1.0) */
#define RED_PROB_MAX           ((uint32_t)1 << RED_PROB_SHIFT)

/* Fixed-point representation of 1/max_p_div */
#define RED_MAXP_ONE(maxp_div)  ((uint32_t)((uint64_t)RED_PROB_MAX / (maxp_div)))

/* ── RED private state ────────────────────────────────────────── */

struct red_priv {
	/* Configuration */
	uint32_t min_th;         /* minimum threshold (packets) */
	uint32_t max_th;         /* maximum threshold (packets) */
	uint32_t max_p;          /* max dropping prob (RED_PROB_SHIFT fixed-point) */
	uint32_t wq_shift;       /* EWMA weight = 1 >> wq_shift (or 1/wq_div) */
	uint32_t wq_div;         /* EWMA weight denominator */
	uint32_t limit;          /* max queue depth (packets) */
	int      ecn_enabled;    /* non-zero = enable ECN marking */
	int      gentle;         /* non-zero = gentle mode (ramp to 2*max_th) */

	/* EWMA state */
	uint32_t avg_q;          /* average queue length (RED_AVG_SHIFT f.p.) */

	/* Drop probability state */
	int      count;          /* packets since last drop (starts at -1) */

	/* Packet FIFO queue */
	void    *queue[RED_QUEUE_HARD_LIMIT];
	int      pkt_len[RED_QUEUE_HARD_LIMIT];
	int      head;
	int      tail;
	int      qlen;            /* current queue length (packets) */

	/* Statistics */
	uint64_t drops;          /* packets dropped (forced drops) */
	uint64_t marks;          /* ECN marks */
	uint64_t early_drops;    /* early random drops */
	uint64_t dequeues;       /* packets successfully dequeued */
};

/* ── EWMA helper ──────────────────────────────────────────────── */

/* Update the exponential weighted moving average of queue length.
 * avg_q = avg_q - (avg_q >> wq_shift) + current_q
 *
 * This implements: avg = (1 - 1/2^N) * avg + current_q
 * where N = wq_shift.  With wq_shift = 9, w_q = 1/512.
 *
 * avg_q is stored with RED_AVG_SHIFT fractional bits for precision.
 */
static void red_ewma(struct red_priv *rp)
{
	uint32_t cur = (uint32_t)rp->qlen << RED_AVG_SHIFT;
	uint32_t avg = rp->avg_q;
	uint32_t decay = avg >> rp->wq_shift;

	/* avg = avg - (avg / 2^N) + current_q */
	if (decay < avg) {
		avg -= decay;
	} else {
		avg = 0;
	}

	/* Add current queue (shifted to fixed-point) */
	avg += cur;

	rp->avg_q = avg;
}

/* ── Drop probability ─────────────────────────────────────────── */

/* Compute the instantaneous drop probability p_b for a given
 * average queue length.
 *
 * Standard RED: p_b = max_p * (avg - min_th) / (max_th - min_th)
 * Gentle RED:   extends linearly from max_th to 2*max_th
 *               where p goes from max_p to 1.0
 *
 * Returns p_b as a fixed-point value in [0, RED_PROB_MAX].
 */
static uint32_t red_prob_pb(struct red_priv *rp)
{
	uint32_t avg_real = rp->avg_q >> RED_AVG_SHIFT;

	/* Below min_th: no drops */
	if (avg_real < rp->min_th)
		return 0;

	/* Between min_th and max_th: linear ramp */
	if (avg_real < rp->max_th) {
		uint32_t q_range = rp->max_th - rp->min_th;
		uint32_t q_diff  = avg_real - rp->min_th;

		/* p_b = max_p * q_diff / q_range */
		return (uint32_t)(((uint64_t)rp->max_p * q_diff) / q_range);
	}

	/* Above max_th: gentle mode or forced drop */
	if (rp->gentle) {
		/* Gentle RED: extend from max_th to 2*max_th */
		uint32_t gentle_max = rp->max_th * 2;
		if (avg_real >= gentle_max)
			return RED_PROB_MAX; /* 1.0 */

		/* p = max_p + (1 - max_p) * (avg - max_th) / max_th */
		uint32_t q_diff = avg_real - rp->max_th;
		uint32_t range  = rp->max_th; /* gentle_max - max_th = max_th */

		uint32_t p_extra = (uint32_t)(((uint64_t)(RED_PROB_MAX - rp->max_p) * q_diff) / range);
		return rp->max_p + p_extra;
	}

	/* Non-gentle: hard limit at max_th means forced drop */
	return RED_PROB_MAX;
}

/* ── ECN marking ──────────────────────────────────────────────── */

/* Mark a packet with ECN Congestion Experienced (CE) codepoint.
 * Returns 1 if marked, 0 if the packet is not ECN-capable.
 *
 * IPv4: ECN field is in IP header byte 1, bits 6-7 (TOS byte).
 *       CE = 0x03.
 * IPv6: Traffic Class byte, bits 6-7.
 */
static int red_ecn_mark(void *pkt, int len)
{
	uint8_t *buf = (uint8_t *)pkt;
	uint8_t *ip;
	uint16_t ethertype;

	/* Need at least Ethernet header (14 bytes) */
	if (len < 14)
		return 0;

	ethertype = (uint16_t)((buf[12] << 8) | buf[13]);

	if (ethertype == 0x8100 && len >= 18) {
		/* 802.1Q VLAN tag */
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

	/* Check ECN field (bits 6-7 of TOS/traffic-class byte) */
	uint8_t ecn = ip[1] & 0x03;
	if (ecn == 0x00 || ecn == 0x03)
		return 0; /* Not ECN-capable, or already CE */

	/* Set CE codepoint */
	ip[1] |= 0x03;
	return 1;
}

/* ── Drop decision ────────────────────────────────────────────── */

/* Decide whether to drop or mark the current packet.
 *
 * Standard RED algorithm:
 *   1. For each arriving packet, compute avg_q (EWMA)
 *   2. If avg_q < min_th → queue the packet
 *   3. If avg_q >= max_th → forced drop (or mark if ECN)
 *   4. If min_th <= avg_q < max_th → compute p_b, then p_a
 *   5. Decide randomly against p_a
 *
 * Returns: 1 = drop/mark, 0 = queue the packet.
 * When *marked is set, the caller should mark (not drop) the packet.
 */
static int red_should_drop(struct red_priv *rp, int *marked)
{
	uint32_t avg_real = rp->avg_q >> RED_AVG_SHIFT;

	*marked = 0;

	/* Step 1: above max_th → forced drop (or forced mark if ECN) */
	if (avg_real >= rp->max_th) {
		rp->count = 0;
		/* If gentle mode and we're in the gentle zone,
		 * red_prob_pb() already returns the right probability. */
		uint32_t pb = red_prob_pb(rp);
		if (pb >= RED_PROB_MAX) {
			if (rp->ecn_enabled) {
				*marked = 1;
				rp->marks++;
			} else {
				rp->drops++;
			}
			return 1;
		}
		/* else fall through to probabilistic decision */
	}

	/* Step 2: below min_th → no action */
	if (avg_real < rp->min_th) {
		rp->count = -1;
		return 0;
	}

	/* Step 3: between min_th and max_th (or gentle zone) */
	rp->count++;

	/* Compute p_b */
	uint32_t pb = red_prob_pb(rp);
	if (pb == 0)
		return 0;

	/* Compute p_a = p_b / (1 - count * p_b)
	 *
	 * This is the standard RED adjustment that increases
	 * probability as more packets arrive without a drop,
	 * ensuring the inter-drop time is approximately uniform.
	 *
	 * For small count*p_b, p_a ≈ p_b.  The adjustment prevents
	 * bursty drops.
	 */
	uint64_t numerator   = (uint64_t)pb << RED_PROB_SHIFT;
	uint32_t count_term  = (uint32_t)rp->count;
	uint64_t denom       = RED_PROB_MAX - (uint64_t)count_term * pb;

	if (denom == 0 || count_term * pb >= RED_PROB_MAX)
		return 1; /* always drop when saturated */

	uint32_t pa = (uint32_t)(numerator / denom);

	/* Step 4: random decision */
	uint32_t rand_val = rng_get_u32() & (RED_PROB_MAX - 1); /* lower bits uniform */

	if (rand_val < pa) {
		/* Drop or mark */
		if (rp->ecn_enabled) {
			*marked = 1;
			rp->marks++;
		} else {
			rp->early_drops++;
		}
		rp->count = 0;
		return 1;
	}

	return 0;
}

/* ── Enqueue ──────────────────────────────────────────────────── */

static int red_enqueue(struct qdisc *q, void *pkt, int len)
{
	struct red_priv *rp = (struct red_priv *)q->priv;

	if (!rp || !pkt)
		return -EINVAL;

	/* Update EWMA average queue length */
	red_ewma(rp);

	/* RED drop decision */
	int marked = 0;
	if (red_should_drop(rp, &marked)) {
		if (marked) {
			/* Try to ECN-mark the packet, then still queue it */
			if (red_ecn_mark(pkt, len)) {
				/* Marked successfully — queue the packet */
				goto queue_pkt;
			}
			/* ECN marking failed — drop it anyway */
		}
		rp->drops++;
		return -EAGAIN; /* tell caller packet was dropped */
	}

queue_pkt:
	/* Check queue capacity */
	if (rp->qlen >= (int)rp->limit) {
		/* Queue full — tail-drop */
		rp->drops++;
		return -EAGAIN;
	}

	if (rp->qlen >= RED_QUEUE_HARD_LIMIT)
		return -EAGAIN;

	rp->queue[rp->tail] = pkt;
	rp->pkt_len[rp->tail] = len;
	rp->tail = (rp->tail + 1) % RED_QUEUE_HARD_LIMIT;
	rp->qlen++;
	return 0;
}

/* ── Dequeue ──────────────────────────────────────────────────── */

static void *red_dequeue(struct qdisc *q)
{
	struct red_priv *rp = (struct red_priv *)q->priv;

	if (!rp || rp->qlen == 0)
		return NULL;

	void *pkt = rp->queue[rp->head];
	rp->head = (rp->head + 1) % RED_QUEUE_HARD_LIMIT;
	rp->qlen--;

	/* Update EWMA on dequeue as well (some implementations do this) */
	red_ewma(rp);

	rp->dequeues++;
	return pkt;
}

/* ── Drop ──────────────────────────────────────────────────────── */

/* Drop the packet at the tail of the queue (tail-drop). */
static int red_drop(struct qdisc *q)
{
	struct red_priv *rp = (struct red_priv *)q->priv;

	if (!rp || rp->qlen == 0)
		return -1;

	rp->tail = (rp->tail - 1 + RED_QUEUE_HARD_LIMIT) % RED_QUEUE_HARD_LIMIT;
	rp->qlen--;
	rp->drops++;
	return 0;
}

/* ── Stats helper ─────────────────────────────────────────────── */

static void red_get_stats(struct qdisc *q, uint64_t *drops, uint64_t *marks,
		   uint64_t *early_drops, uint64_t *dequeues)
{
	struct red_priv *rp = (struct red_priv *)q->priv;
	if (!rp) {
		if (drops)       *drops = 0;
		if (marks)       *marks = 0;
		if (early_drops) *early_drops = 0;
		if (dequeues)    *dequeues = 0;
		return;
	}
	if (drops)       *drops       = rp->drops;
	if (marks)       *marks       = rp->marks;
	if (early_drops) *early_drops = rp->early_drops;
	if (dequeues)    *dequeues    = rp->dequeues;
}

/* ── Get/set parameters ───────────────────────────────────────── */

/* Return the current average queue length (in packets, real not fixed-point). */
static uint32_t red_get_avg_q(struct qdisc *q)
{
	struct red_priv *rp = (struct red_priv *)q->priv;
	if (!rp)
		return 0;
	return rp->avg_q >> RED_AVG_SHIFT;
}

/* Get the current queue depth (packets). */
static int red_qlen(struct qdisc *q)
{
	struct red_priv *rp = (struct red_priv *)q->priv;
	if (!rp)
		return 0;
	return rp->qlen;
}

/* ── Create ────────────────────────────────────────────────────── */

/* Forward declaration of stats callback */
static void red_fill_stats(struct qdisc *q, struct tc_stats *st);

struct qdisc *red_create(const struct red_spec *spec)
{
	struct qdisc *q = (struct qdisc *)kmalloc(sizeof(struct qdisc));
	if (!q)
		return NULL;

	struct red_priv *rp = (struct red_priv *)kmalloc(sizeof(struct red_priv));
	if (!rp) {
		kfree(q);
		return NULL;
	}

	memset(rp, 0, sizeof(*rp));

	/* Apply configuration or defaults */
	if (spec) {
		rp->min_th      = spec->min_th ? spec->min_th : RED_DEFAULT_MIN_TH;
		rp->max_th      = spec->max_th ? spec->max_th : RED_DEFAULT_MAX_TH;
		rp->max_p       = spec->max_p  ? RED_MAXP_ONE(spec->max_p) :
		                   RED_MAXP_ONE(RED_DEFAULT_MAX_P);
		rp->wq_div      = spec->wq_div ? spec->wq_div : RED_DEFAULT_WQ_DIV;
		rp->limit       = spec->limit  ? spec->limit  : RED_DEFAULT_LIMIT;
		rp->ecn_enabled = spec->ecn;
	} else {
		rp->min_th      = RED_DEFAULT_MIN_TH;
		rp->max_th      = RED_DEFAULT_MAX_TH;
		rp->max_p       = RED_MAXP_ONE(RED_DEFAULT_MAX_P);
		rp->wq_div      = RED_DEFAULT_WQ_DIV;
		rp->limit       = RED_DEFAULT_LIMIT;
		rp->ecn_enabled = 0;
	}

	/* Compute wq_shift from wq_div (find highest set bit as shift). */
	int shift = 0;
	uint32_t div = rp->wq_div;
	while (div > 1) {
		div >>= 1;
		shift++;
	}
	rp->wq_shift = (uint32_t)shift;

	/* Clamp thresholds: min_th must be < max_th */
	if (rp->min_th >= rp->max_th) {
		rp->max_th = rp->min_th * 3;
		if (rp->max_th == rp->min_th)
			rp->max_th = rp->min_th + 1;
	}

	/* Clamp limit */
	if (rp->limit > RED_QUEUE_HARD_LIMIT)
		rp->limit = RED_QUEUE_HARD_LIMIT;

	/* Initial EWMA state: avg_q starts at 0 */
	rp->avg_q = 0;
	rp->count = -1;
	rp->gentle = 1; /* enable gentle mode by default */

	q->type    = QDISC_RED;
	q->priv    = rp;
	q->enqueue = red_enqueue;
	q->dequeue = red_dequeue;
	q->drop    = red_drop;
	q->get_stats      = red_fill_stats;
	q->get_class_stats = NULL;

	kprintf("[red] RED qdisc created: min_th=%u max_th=%u limit=%u ecn=%d\n",
	    rp->min_th, rp->max_th, rp->limit, rp->ecn_enabled);

	return q;
}

/* ── Stats callback ────────────────────────────────────────────── */

static void red_fill_stats(struct qdisc *q, struct tc_stats *st)
{
	struct red_priv *rp = (struct red_priv *)q->priv;
	if (!rp || !st) return;
	memset(st, 0, sizeof(*st));
	st->drops      = (uint32_t)(rp->drops + rp->early_drops);
	st->overlimits = (uint32_t)rp->marks;
	st->qlen       = (uint32_t)rp->qlen;
	st->backlog    = rp->qlen * 1500;
}

/* ── Module registration ──────────────────────────────────────── */

#include "module.h"
MODULE_LICENSE("MIT");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("RED (Random Early Detection) AQM qdisc");
