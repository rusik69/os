#ifndef TCP_BBR_H
#define TCP_BBR_H

#include "types.h"

/* ── BBR per-connection state (embedded directly in struct tcp_conn) ── */

struct bbr_data {
    /* Estimated bottleneck bandwidth (bytes per tick) */
    uint32_t bw;
    uint32_t bw_hi;

    /* Minimum round-trip time (in ticks) */
    uint32_t min_rtt;
    uint64_t min_rtt_stamp;

    /* Round tracking */
    uint64_t round_start;
    uint16_t round_count;
    uint32_t round_delivered;
    uint32_t delivered;
    uint64_t delivered_tick;

    /* State machine */
    uint8_t  state;
    uint8_t  probe_bw_phase;
    uint8_t  startup_rounds;

    /* Pacing rate (bytes per tick) */
    uint32_t pacing_rate;

    /* Target cwnd */
    uint32_t target_cwnd;

    /* PROBE_RTT timing */
    uint64_t probe_rtt_done_stamp;
    uint8_t  probe_rtt_round_done;
    uint8_t  packet_conservation;

    /* ACK rate sampling */
    uint32_t ack_epoch_bytes;
    uint64_t ack_epoch_tick_start;
    int      bw_initialized;
};

/* ── BBR API ─────────────────────────────────────────────────────────── */

/* Allocate and initialize BBR state */
void     bbr_init(struct bbr_data *b);

/* Called on every ACK with ACKed bytes count and current RTT */
void     bbr_on_ack(struct bbr_data *b, uint32_t acked_bytes,
                     uint32_t cwnd_segments, uint32_t rtt_ticks, uint64_t now);

/* Called on packet loss / retransmit */
void     bbr_on_loss(struct bbr_data *b);

/* Return target congestion window (segments) */
uint32_t bbr_get_cwnd(struct bbr_data *b, uint32_t current_cwnd);

/* Return pacing rate (bytes per tick) */
uint32_t bbr_get_pacing_rate(struct bbr_data *b);

/* Return BBR state as string */
const char *bbr_state_str(struct bbr_data *b);

/* Dump BBR state to kernel log */
void     bbr_dump(struct bbr_data *b);

/* Check whether BBR is actively running with estimates */
int      bbr_is_active(struct bbr_data *b);

#endif /* TCP_BBR_H */
