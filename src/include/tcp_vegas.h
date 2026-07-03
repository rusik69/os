#ifndef TCP_VEGAS_H
#define TCP_VEGAS_H

#include "types.h"

/*
 * TCP Vegas: Delay-based congestion control (Brakmo & Peterson, 1995).
 *
 * Instead of waiting for packet loss, Vegas uses RTT measurements to detect
 * incipient congestion before buffers overflow.  It maintains a "BaseRTT"
 * (minimum observed RTT) and compares the current Expected rate (cwnd/BaseRTT)
 * with the Actual rate (cwnd/RTT).  If the difference exceeds thresholds α/β,
 * the window is adjusted.
 *
 * Three operating regions:
 *   diff < α     → increase window   (under-utilised)
 *   α ≤ diff ≤ β → hold window       (optimal)
 *   diff > β     → decrease window   (congestion building)
 *
 * where diff ≈ cwnd * (RTT - BaseRTT) / RTT
 */

/* Vegas thresholds (in segments) */
#define VEGAS_ALPHA    2        /* low threshold */
#define VEGAS_BETA     4        /* high threshold */
#define VEGAS_GAMMA    1        /* slow-start exit threshold */

/* Vegas per-connection state (embedded directly in struct tcp_conn) */
struct vegas_data {
    uint32_t base_rtt;          /* minimum observed RTT (in ticks) */
    uint32_t current_rtt;       /* latest RTT sample */
    uint32_t cwnd;              /* current congestion window */
    uint32_t ssthresh;          /* slow-start threshold */
    uint64_t last_ack_tick;     /* tick when last ACK was processed */
    int      initialised;
};

/* ── Internal API ──────────────────────────────────────────────────── */

void     vegas_init(struct vegas_data *v);
void     vegas_update_rtt(struct vegas_data *v, uint32_t rtt_ticks);
uint32_t vegas_update(struct vegas_data *v, uint32_t cwnd,
                      int acked_segments, int in_slow_start);
void     vegas_on_loss(struct vegas_data *v, uint32_t current_cwnd);
uint32_t vegas_get_cwnd(struct vegas_data *v);
void     vegas_set_cwnd(struct vegas_data *v, uint32_t cwnd);

/* ── TCP congestion control callbacks ─────────────────────────────────
 *
 * These are the external entry points that the TCP stack (or a pluggable
 * cc_ops framework) calls during connection processing.  They take a
 * pointer to struct tcp_conn (passed as void *sk for generic interface).
 */

int      tcp_vegas_init(void *sk);
int      tcp_vegas_cong_avoid(void *sk);
uint32_t tcp_vegas_ssthresh(void *sk);
int      tcp_vegas_acked(void *sk, uint32_t acked);

#endif /* TCP_VEGAS_H */
