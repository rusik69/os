/* tcp_westwood.c — TCP Westwood+ (bandwidth estimation) */

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

/* Westwood per-connection state */
struct westwood_data {
    /* Bandwidth estimation */
    uint32_t bw_est;                /* estimated bandwidth (bytes/tick) */
    uint32_t sample_bw;             /* current sample bandwidth */
    uint64_t last_ack_tick;         /* tick of last ACK */
    uint32_t acked_bytes;           /* bytes ACKed in current sample period */
    uint32_t sample_count;          /* samples taken */

    /* RTT */
    uint32_t rtt_min;               /* minimum RTT in ticks */
    uint32_t rtt;                   /* current RTT */

    /* Filtering */
    uint32_t bw_filtered;           /* exponentially filtered BW */
#define BW_FILTER_SHIFT   3         /* weight = 1/8 for new sample */

    /* Window */
    uint32_t cwnd;
    uint32_t ssthresh;

    int initialised;
};

/* Filter coefficients */
#define WESTWOOD_BW_WEIGHT      8   /* 1/8 for new sample, 7/8 for history */

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

/* ── Implement: tcp_westwood_cong_avoid ────────────────── */
int tcp_westwood_cong_avoid(void *sk) { (void)sk; return 0; }
/* ── Implement: tcp_westwood_ssthresh ────────────────── */
uint32_t tcp_westwood_ssthresh(void *sk) { (void)sk; return 2; }
/* ── Implement: tcp_westwood_acked ────────────────── */
int tcp_westwood_acked(void *sk, uint32_t acked)
{
    if (!sk) {
        kprintf("[tcp_westwood] tcp_westwood_acked: NULL sk\n");
        return -EINVAL;
    }
    kprintf("[tcp_westwood] tcp_westwood_acked: sk=%p acked=%u (stub)\n", sk, acked);
    return -EOPNOTSUPP;
}
