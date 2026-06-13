/* tcp_vegas.c — TCP Vegas (early congestion detection via RTT) */

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

/* Vegas thresholds (in segments) */
#define VEGAS_ALPHA    2        /* low threshold */
#define VEGAS_BETA     4        /* high threshold */
#define VEGAS_GAMMA    1        /* slow-start threshold (exit threshold) */

/* Vegas per-connection state */
struct vegas_data {
    uint32_t base_rtt;          /* minimum observed RTT (in ticks) */
    uint32_t current_rtt;       /* latest RTT sample */
    uint32_t cwnd;              /* current congestion window */
    uint32_t ssthresh;          /* slow-start threshold */
    uint64_t last_ack_tick;     /* tick when last ACK was processed */
    int      initialised;
};

/* Phase of slow-start: detect first congestion via RTT increase */
#define VEGAS_EXPECTED_RATE(cwnd, base_rtt)  ((cwnd) * 1000 / (base_rtt))

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
