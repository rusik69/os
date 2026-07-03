/* tcp_vegas.c — TCP Vegas (early congestion detection via RTT) */

#include "tcp_vegas.h"
#include "net_internal.h"   /* struct tcp_conn */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * TCP Vegas: Delay-based congestion control (Brakmo & Peterson, 1995).
 *
 * Instead of waiting for packet loss, Vegas uses RTT measurements to detect
 * incipient congestion before buffers overflow.  It maintains a "BaseRTT"
 * (minimum observed RTT) and compares the current Expected rate (cwnd/BaseRTT)
 * with the Actual rate (cwnd/RTT).  If the difference exceeds thresholds α/β,
 * the window is adjusted.
 *
 * Key features:
 *   - Congestion avoidance: compare Expected vs Actual throughput
 *   - Linear window increase/decrease (not multiplicative)
 *   - Faster loss detection via early RTT signalling
 *
 * Three operating regions:
 *   diff < α     → increase window     (under-utilised)
 *   α ≤ diff ≤ β → hold window           (optimal)
 *   diff > β     → decrease window     (congestion building)
 *
 * where diff = (Expected - Actual) * BaseRTT ≈ cwnd * (RTT - BaseRTT) / RTT
 */

void vegas_init(struct vegas_data *v)
{
    if (!v || v->initialised) return;
    memset(v, 0, sizeof(*v));
    v->base_rtt = 0xFFFFFFFF;       /* initialise to max */
    v->cwnd = 2;                    /* initial cwnd = 2 segments */
    v->ssthresh = 0x7FFFFFFF;
    v->initialised = 1;
}

/* Update RTT sample */
void vegas_update_rtt(struct vegas_data *v, uint32_t rtt_ticks)
{
    if (!v || !v->initialised || rtt_ticks == 0) return;

    v->current_rtt = rtt_ticks;

    /* Update BaseRTT (minimum observed) */
    if (rtt_ticks < v->base_rtt)
        v->base_rtt = rtt_ticks;
}

/* Vegas congestion avoidance update (called per ACK) */
uint32_t vegas_update(struct vegas_data *v, uint32_t cwnd,
                      int acked_segments, int in_slow_start)
{
    (void)acked_segments;
    if (!v || !v->initialised) return cwnd;
    if (v->base_rtt == 0 || v->base_rtt == 0xFFFFFFFF || v->current_rtt == 0)
        return cwnd;  /* not enough data yet */

    /* Expected throughput (cwnd / BaseRTT) vs Actual (cwnd / RTT) */
    uint32_t expected = (cwnd * v->base_rtt);  /* scaled */
    uint32_t actual   = (cwnd * v->current_rtt);
    int32_t diff;

    if (v->current_rtt > v->base_rtt) {
        /* diff = (expected - actual) / (RTT * base_rtt?) */
        /* Simplified: diff ≈ cwnd * (RTT - BaseRTT) / RTT */
        diff = (int32_t)((cwnd * (v->current_rtt - v->base_rtt)) / v->current_rtt);
    } else {
        diff = 0;
    }

    if (in_slow_start) {
        /* Vegas slow-start: exit when diff > gamma */
        if (diff > VEGAS_GAMMA) {
            /* Exit slow start, switch to congestion avoidance */
            v->ssthresh = cwnd;
            return cwnd;  /* hold cwnd, let caller switch */
        }
        /* Exponential increase during slow-start */
        return cwnd + (uint32_t)acked_segments;
    }

    /* Congestion avoidance */
    if (diff < (int32_t)VEGAS_ALPHA) {
        /* Under-utilised: increase window */
        if (cwnd < v->ssthresh) {
            cwnd += (uint32_t)acked_segments;
        } else {
            cwnd++;  /* additive increase */
        }
    } else if (diff > (int32_t)VEGAS_BETA) {
        /* Congestion building: decrease window */
        cwnd--;
    }
    /* else: α ≤ diff ≤ β → hold window unchanged */

    if (cwnd < 2) cwnd = 2;     /* minimum window */

    return cwnd;
}

/* Vegas on packet loss (RTO/dupack) */
void vegas_on_loss(struct vegas_data *v, uint32_t current_cwnd)
{
    if (!v || !v->initialised) return;

    /* Vegas responds to loss by reducing window */
    v->ssthresh = current_cwnd / 2;
    v->cwnd = v->ssthresh;

    /* Reset RTT measurements (loss often means route change) */
    /* Keep base_rtt but let it re-converge */
}

uint32_t vegas_get_cwnd(struct vegas_data *v)
{
    if (!v || !v->initialised) return 2;
    return v->cwnd;
}

void vegas_set_cwnd(struct vegas_data *v, uint32_t cwnd)
{
    if (!v || !v->initialised) return;
    v->cwnd = cwnd;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  TCP congestion control callbacks
 *
 *  These are called by the TCP stack (or by a pluggable cc_ops framework)
 *  during connection lifecycle.  The void *sk parameter is a pointer to
 *  the corresponding struct tcp_conn.
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── tcp_vegas_init ──────────────────────────────────────────────────
 *
 * Initialise the per-connection Vegas state when the connection is
 * established or when the CC algorithm is switched to Vegas.
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_vegas_init(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    vegas_init(&conn->vegas);
    return 0;
}

/* ── tcp_vegas_cong_avoid ────────────────────────────────────────────
 *
 * Called per ACK during congestion avoidance to update the congestion
 * window using the Vegas delay-based algorithm.  Compares expected
 * throughput (cwnd/BaseRTT) with actual throughput (cwnd/RTT) and
 * adjusts the window when the difference exceeds the α/β thresholds.
 *
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_vegas_cong_avoid(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    conn->cwnd = vegas_update(&conn->vegas, conn->cwnd, 1, 0);
    return 0;
}

/* ── tcp_vegas_ssthresh ──────────────────────────────────────────────
 *
 * Called when a loss event occurs (dupack or timeout) to compute the
 * new slow-start threshold.  Vegas uses standard Reno halving:
 *
 *   ssthresh = max(cwnd / 2, 2)
 *
 * Returns the new ssthresh in segments (minimum 2).
 */
uint32_t tcp_vegas_ssthresh(void *sk)
{
    if (!sk)
        return 2;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct vegas_data *v = &conn->vegas;

    vegas_on_loss(v, conn->cwnd);
    return v->ssthresh;
}

/* ── tcp_vegas_acked ─────────────────────────────────────────────────
 *
 * Called when an ACK arrives.  Updates the Vegas RTT measurement via
 * vegas_update_rtt() using the current smoothed RTT from conn->srtt,
 * then advances the congestion window through vegas_update().
 *
 * @sk     Pointer to struct tcp_conn
 * @acked  Number of bytes newly ACKed by this segment (unused by Vegas)
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_vegas_acked(void *sk, uint32_t acked)
{
    (void)acked;
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct vegas_data *v = &conn->vegas;

    /* Convert srtt from scaled value (scaled by 8) to ticks */
    uint32_t rtt_ticks = 0;
    if (conn->srtt > 0)
        rtt_ticks = (uint32_t)((uint32_t)conn->srtt / 8);

    /* Update RTT measurement for Vegas expected/actual comparison */
    vegas_update_rtt(v, rtt_ticks);

    /* Update congestion window using Vegas delay-based algorithm */
    conn->cwnd = vegas_update(v, conn->cwnd, 1, 0);

    return 0;
}
