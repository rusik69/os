/* tcp_westwood.c — TCP Westwood+ (bandwidth estimation-based CC) */

#include "tcp_westwood.h"
#include "net_internal.h"   /* struct tcp_conn */
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/*
 * TCP Westwood+: Bandwidth-estimation based congestion control.
 *
 * Westwood+ (RFC 6298-ish, extended from Westwood) uses end-to-end bandwidth
 * estimation to set the congestion window and slow-start threshold after a
 * loss event.  Instead of halving cwnd on loss (standard Reno), it sets
 * cwnd and ssthresh based on the estimated available bandwidth (BWE).
 *
 * Key idea:
 *   - Estimate the connection's bandwidth by monitoring ACK arrivals:
 *     BWE = (acked_bytes * TIMER_FREQ) / (current_tick - last_ack_tick)
 *   - On loss (dupack or timeout):
 *     ssthresh = max(BWE * RTTmin / seg_size, 2)
 *     cwnd = ssthresh
 *
 * This gives better performance over wireless/lossy links where packet loss
 * is not congestion-related.
 */

void westwood_init(struct westwood_data *w)
{
    if (!w || w->initialised) return;
    memset(w, 0, sizeof(*w));
    w->cwnd = 10;
    w->ssthresh = 0x7FFFFFFF;
    w->rtt_min = 0xFFFFFFFF;
    w->bw_filtered = 100;               /* initial 100 bytes/tick */
    w->last_ack_tick = 0;
    w->initialised = 1;
}

/* Update bandwidth estimate on ACK */
void westwood_on_ack(struct westwood_data *w, uint32_t acked,
                     uint64_t now_tick, uint32_t rtt_ticks)
{
    if (!w || !w->initialised) return;

    w->acked_bytes += acked;
    w->rtt = rtt_ticks;

    /* Update min RTT */
    if (rtt_ticks > 0 && rtt_ticks < w->rtt_min)
        w->rtt_min = rtt_ticks;

    /* Sample bandwidth estimate */
    if (w->last_ack_tick > 0 && now_tick > w->last_ack_tick) {
        uint32_t delta = (uint32_t)(now_tick - w->last_ack_tick);
        if (delta > 0) {
            /* Bandwidth = acked_bytes / time_delta (in ticks) */
            uint32_t sample = (w->acked_bytes * TIMER_FREQ) / delta;

            /* Exponentially weighted moving average */
            if (w->sample_count == 0) {
                w->bw_filtered = sample;
            } else {
                w->bw_filtered = ((w->bw_filtered * (WESTWOOD_BW_WEIGHT - 1)) +
                                  sample) / WESTWOOD_BW_WEIGHT;
            }
            w->sample_count++;
        }
    }

    w->last_ack_tick = now_tick;
    w->acked_bytes = 0;
    w->bw_est = w->bw_filtered;
}

/* Update cwnd in congestion avoidance (additive increase) */
uint32_t westwood_update(struct westwood_data *w, uint32_t cwnd,
                         int acked_segments)
{
    if (!w || !w->initialised) return cwnd;

    if (cwnd < w->ssthresh) {
        /* Slow start */
        cwnd += (uint32_t)acked_segments;
    } else {
        /* Congestion avoidance */
        if (cwnd == 0) cwnd = 1;
        cwnd += (uint32_t)acked_segments / cwnd;
        if (cwnd < 2) cwnd = 2;
    }

    w->cwnd = cwnd;
    return cwnd;
}

/* Handle loss event (dupack or timeout) */
void westwood_on_loss(struct westwood_data *w, uint32_t current_cwnd,
                      uint32_t seg_size)
{
    if (!w || !w->initialised) return;

    /*
     * Set ssthresh based on bandwidth estimate:
     *   ssthresh = (BWE * RTT_min) / segment_size
     * but at least 2 segments.
     */
    uint32_t estimated_cwnd;

    if (w->bw_filtered > 0 && w->rtt_min > 0 && w->rtt_min != 0xFFFFFFFF && seg_size > 0) {
        estimated_cwnd = (w->bw_filtered * w->rtt_min) / seg_size;
    } else {
        /* Fallback: standard Reno halving */
        estimated_cwnd = current_cwnd / 2;
    }

    if (estimated_cwnd < 2)
        estimated_cwnd = 2;

    w->ssthresh = estimated_cwnd;
    w->cwnd = w->ssthresh;
}

uint32_t westwood_get_cwnd(struct westwood_data *w)
{
    if (!w || !w->initialised) return 10;
    return w->cwnd;
}

void westwood_set_cwnd(struct westwood_data *w, uint32_t cwnd)
{
    if (!w || !w->initialised) return;
    w->cwnd = cwnd;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  TCP congestion control callbacks
 *
 *  These are called by the TCP stack (or by a pluggable cc_ops framework)
 *  during connection lifecycle.  The void *sk parameter is a pointer to
 *  the corresponding struct tcp_conn.
 * ═══════════════════════════════════════════════════════════════════════ */

/* ── tcp_westwood_init ──────────────────────────────────────────────
 *
 * Initialise the per-connection Westwood state when the connection is
 * established or when the CC algorithm is switched to Westwood.
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_westwood_init(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    westwood_init(&conn->westwood);
    return 0;
}

/* ── tcp_westwood_cong_avoid ────────────────────────────────────────
 *
 * Called per ACK during congestion avoidance to increase the congestion
 * window.  Uses westwood_update() which applies slow-start (exponential)
 * when below ssthresh and AIMD (linear) when above.
 *
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_westwood_cong_avoid(void *sk)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    conn->cwnd = westwood_update(&conn->westwood, conn->cwnd, 1);
    return 0;
}

/* ── tcp_westwood_ssthresh ──────────────────────────────────────────
 *
 * Called when a loss event occurs (dupack or timeout) to compute the
 * new slow-start threshold.  Uses the Westwood bandwidth estimate:
 *
 *   ssthresh = (BWE * RTT_min) / segment_size
 *
 * where segment_size defaults to the MSS.  If bandwidth estimation is
 * unavailable (no samples yet), falls back to Reno halving (cwnd / 2).
 *
 * Returns the new ssthresh in segments (minimum 2).
 */
uint32_t tcp_westwood_ssthresh(void *sk)
{
    if (!sk)
        return 2;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct westwood_data *w = &conn->westwood;

    /* Default segment size: 1460 bytes (standard Ethernet MSS) */
    uint32_t seg_size = 1460;

    westwood_on_loss(w, conn->cwnd, seg_size);
    return w->ssthresh;
}

/* ── tcp_westwood_acked ─────────────────────────────────────────────
 *
 * Called when an ACK arrives.  Updates the Westwood bandwidth estimate
 * via westwood_on_ack() using the number of bytes ACKed and the current
 * RTT sample.  Also advances the congestion window through
 * westwood_update().
 *
 * @sk     Pointer to struct tcp_conn
 * @acked  Number of bytes newly ACKed by this segment
 * Returns 0 on success, -EINVAL on NULL sk.
 */
int tcp_westwood_acked(void *sk, uint32_t acked)
{
    if (!sk)
        return -EINVAL;

    struct tcp_conn *conn = (struct tcp_conn *)sk;
    struct westwood_data *w = &conn->westwood;

    /* Get the current tick for bandwidth timestamp */
    uint64_t now_tick = timer_get_ticks();

    /* Convert srtt from scaled value (scaled by 8) to ticks */
    uint32_t rtt_ticks = 0;
    if (conn->srtt > 0)
        rtt_ticks = (uint32_t)((uint32_t)conn->srtt / 8);

    /* Update bandwidth estimate */
    westwood_on_ack(w, acked, now_tick, rtt_ticks);

    /* Update congestion window */
    conn->cwnd = westwood_update(w, conn->cwnd, 1);

    return 0;
}
