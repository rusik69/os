#ifndef TCP_WESTWOOD_H
#define TCP_WESTWOOD_H

#include "types.h"

/* ── TCP Westwood+ per-connection state ───────────────────────────────
 *
 * Westwood+ uses end-to-end bandwidth estimation to set the congestion
 * window and slow-start threshold after a loss event, rather than the
 * standard Reno halving.  This gives better throughput over wireless or
 * lossy links where packet loss is not congestion-related.
 *
 * Bandwidth estimation:
 *   BWE = (acked_bytes * TIMER_FREQ) / (current_tick - last_ack_tick)
 *
 * On loss:
 *   ssthresh = max(BWE * RTTmin / seg_size, 2)
 *   cwnd = ssthresh
 */

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

    /* Window */
    uint32_t cwnd;
    uint32_t ssthresh;

    int initialised;
};

/* Filter coefficient weight (1/8 for new sample, 7/8 for history) */
#define WESTWOOD_BW_WEIGHT      8

/* ── Internal API ──────────────────────────────────────────────────── */

void     westwood_init(struct westwood_data *w);
void     westwood_on_ack(struct westwood_data *w, uint32_t acked,
                         uint64_t now_tick, uint32_t rtt_ticks);
uint32_t westwood_update(struct westwood_data *w, uint32_t cwnd,
                         int acked_segments);
void     westwood_on_loss(struct westwood_data *w, uint32_t current_cwnd,
                          uint32_t seg_size);
uint32_t westwood_get_cwnd(struct westwood_data *w);
void     westwood_set_cwnd(struct westwood_data *w, uint32_t cwnd);

/* ── TCP congestion control callbacks ─────────────────────────────────
 *
 * These are the external entry points that the TCP stack (or a pluggable
 * cc_ops framework) calls during connection processing.  They take a
 * pointer to struct tcp_conn (passed as void *sk for generic interface).
 */

int      tcp_westwood_init(void *sk);
int      tcp_westwood_cong_avoid(void *sk);
uint32_t tcp_westwood_ssthresh(void *sk);
int      tcp_westwood_acked(void *sk, uint32_t acked);

#endif /* TCP_WESTWOOD_H */
