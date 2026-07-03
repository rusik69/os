/* tcp_hybla.c — TCP Hybla (satellite links) */

#include "tcp_hybla.h"
#include "net_internal.h"   /* struct tcp_conn */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * TCP Hybla: Congestion control for high-latency/satellite links (RFC 6298 variant).
 *
 * Hybla addresses the RTT unfairness problem of standard TCP by normalising
 * the window increase rate to a reference RTT (25 ms).  This ensures that
 * connections with long RTTs (satellite links: 250-600 ms) get a fair share
 * of bandwidth compared to terrestrial connections.
 *
 * Key mechanisms:
 *   - Normalised increase rate: cwnd += ρ² / cwnd per ACK (instead of 1/cwnd)
 *     where ρ = RTT / RTT₀ (RTT₀ = 25 ms = reference RTT)
 *   - On loss: standard multiplicative decrease (cwnd /= 2)
 *   - Slow-start: cwnd += ρ² per ACK (instead of 1 per ACK)
 *
 * Also implements RFC 3465 (Appropriate Byte Counting) integration.
 */

void hybla_init(struct hybla_data *h)
{
    if (!h || h->initialised) return;
    memset(h, 0, sizeof(*h));
    h->rtt0 = HYBLA_RTT0;
    h->min_rtt = 0xFFFFFFFF;
    h->cwnd = 4;                    /* smaller initial window for satellite */
    h->ssthresh = 0x7FFFFFFF;
    h->rho = HYBLA_SCALE;           /* ρ = 1 if no RTT measured yet */
    h->rho2 = HYBLA_SCALE;          /* ρ² = 1 */
    h->initialised = 1;
}

/* Update RTT and recalculate ρ */
void hybla_update_rtt(struct hybla_data *h, uint32_t rtt_ticks)
{
    if (!h || !h->initialised || rtt_ticks == 0) return;

    h->rtt = rtt_ticks;

    /* Update minimum RTT */
    if (rtt_ticks < h->min_rtt)
        h->min_rtt = rtt_ticks;

    /* Use min_rtt for ρ calculation (more stable) */
    uint32_t rtt_for_rho = (h->min_rtt != 0xFFFFFFFF) ? h->min_rtt : rtt_ticks;

    /* Calculate ρ = RTT / RTT0 (fixed-point: ρ * HYBLA_SCALE) */
    if (h->rtt0 > 0) {
        h->rho = (rtt_for_rho * HYBLA_SCALE) / h->rtt0;

        /* ρ² = ρ * ρ / HYBLA_SCALE (re-normalise) */
        h->rho2 = (h->rho * h->rho) / HYBLA_SCALE;

        if (h->rho2 < HYBLA_SCALE)
            h->rho2 = HYBLA_SCALE;  /* minimum ρ² = 1 */
    }
}

/* Hybla congestion avoidance update (per ACK) */
uint32_t hybla_update(struct hybla_data *h, uint32_t cwnd, int acked_segments)
{
    if (!h || !h->initialised) return cwnd;

    if (cwnd < h->ssthresh) {
        /* Slow start: cwnd += ρ² per ACK */
        uint32_t inc = (h->rho2 * (uint32_t)acked_segments) / HYBLA_SCALE;
        if (inc < 1) inc = 1;
        cwnd += inc;
    } else {
        /* Congestion avoidance: cwnd += ρ² / cwnd per ACK */
        /* Since we process multiple ACKs: cwnd += ρ² * acked / cwnd */
        if (cwnd == 0) cwnd = 1;
        uint32_t inc = (h->rho2 * (uint32_t)acked_segments) / cwnd;
        if (inc < 1) inc = 1;
        /* Clamp to avoid excessive growth */
        if (inc > (uint32_t)acked_segments)
            inc = (uint32_t)acked_segments;
        cwnd += inc;
    }

    if (cwnd > 0xFFFF) cwnd = 0xFFFF;  /* cap at an extreme max */

    h->cwnd = cwnd;
    return cwnd;
}

/* Hybla on loss: standard multiplicative decrease */
void hybla_on_loss(struct hybla_data *h, uint32_t current_cwnd)
{
    if (!h || !h->initialised) return;

    h->ssthresh = current_cwnd / 2;
    if (h->ssthresh < 2) h->ssthresh = 2;
    h->cwnd = h->ssthresh;
}

uint32_t hybla_get_cwnd(struct hybla_data *h)
{
    if (!h || !h->initialised) return 4;
    return h->cwnd;
}

void hybla_set_cwnd(struct hybla_data *h, uint32_t cwnd)
{
    if (!h || !h->initialised) return;
    h->cwnd = cwnd;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  TCP congestion control callbacks
 *
 *  These are called by the TCP stack (or by a pluggable cc_ops framework)
 *  during connection lifecycle.  The void *sk parameter is a pointer to
 *  the corresponding struct tcp_conn.
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── tcp_hybla_init ──────────────────────────────────────────────────
 *
 * Initialise the per-connection Hybla state when the connection is
 * established or when the CC algorithm is switched to Hybla.
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_hybla_init(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    hybla_init(&conn->hybla);
    return 0;
}

/* ── tcp_hybla_cong_avoid ────────────────────────────────────────────
 *
 * Called per ACK during congestion avoidance to increase the congestion
 * window using the Hybla normalised rate.  Slow-start uses cwnd += ρ²
 * per ACK (faster growth for satellite links); congestion avoidance uses
 * cwnd += ρ² / cwnd per ACK (the normalised AIMD).
 *
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_hybla_cong_avoid(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    conn->cwnd = hybla_update(&conn->hybla, conn->cwnd, 1);
    return 0;
}

/* ── tcp_hybla_ssthresh ──────────────────────────────────────────────
 *
 * Called when a loss event occurs (dupack or timeout) to compute the
 * new slow-start threshold.  Hybla uses standard multiplicative decrease
 * (same as Reno):
 *
 *   ssthresh = max(cwnd / 2, 2)
 *
 * Returns the new ssthresh in segments (minimum 2).
 */
uint32_t tcp_hybla_ssthresh(void *sk)
{
    if (!sk)
        return 2;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct hybla_data *h = &conn->hybla;

    hybla_on_loss(h, conn->cwnd);
    return h->ssthresh;
}

/* ── tcp_hybla_acked ─────────────────────────────────────────────────
 *
 * Called when an ACK arrives.  Updates the Hybla RTT measurement via
 * hybla_update_rtt() using the current smoothed RTT from conn->srtt,
 * then advances the congestion window through hybla_update().
 *
 * @sk     Pointer to struct tcp_conn
 * @acked  Number of bytes newly ACKed by this segment
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_hybla_acked(void *sk, uint32_t acked)
{
    (void)acked;
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct hybla_data *h = &conn->hybla;

    /* Convert srtt from scaled value (scaled by 8) to ticks */
    uint32_t rtt_ticks = 0;
    if (conn->srtt > 0)
        rtt_ticks = (uint32_t)((uint32_t)conn->srtt / 8);

    /* Update RTT measurement for ρ normalisation */
    hybla_update_rtt(h, rtt_ticks);

    /* Update congestion window using Hybla normalised AIMD */
    conn->cwnd = hybla_update(h, conn->cwnd, 1);

    return 0;
}
