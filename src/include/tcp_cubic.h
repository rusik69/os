#ifndef TCP_CUBIC_H
#define TCP_CUBIC_H

#include "types.h"

/* ── CUBIC per-connection state (embedded directly in struct tcp_conn) ── */

struct cubic_data {
    uint32_t wmax;              /* W_max: cwnd at last congestion event */
    uint64_t epoch_start;       /* tick when the current epoch started */
    uint32_t origin_point;      /* origin point for cubic growth */
    int      use_cubic;         /* 1 = using CUBIC algorithm (epoch established) */

    /* ── Hybrid slow start (RFC 8312 §3) ─────────────────────────── */
    uint64_t hystart_last_ack_ms;    /* ms timestamp of last ACK during slow start */
    uint32_t hystart_lowest_rtt_ms;  /* lowest RTT (ms) observed in current SS */
    uint32_t hystart_curr_rtt_ms;    /* min RTT in current RTT round (ms) */
    uint16_t hystart_round_cnt;      /* RTT rounds completed in slow start */
    uint16_t hystart_samples;        /* RTT samples taken in slow start */
    uint8_t  hystart_found;          /* 1 = exit slow start signal found */
    uint8_t  hystart_detect;         /* 0=NONE, 1=ACK_TRAIN, 2=DELAY */
};

/* ── CUBIC API ─────────────────────────────────────────────────────────── */

/* Allocate and initialize CUBIC state */
void     cubic_init(struct cubic_data *c);

/*
 * Compute target congestion window using the cubic function.
 * Called per ACK during congestion avoidance.
 * Returns target cwnd in segments.
 */
uint32_t cubic_update(struct cubic_data *c, uint32_t cwnd,
                      uint64_t now, uint32_t rtt_ticks);

/*
 * Handle a congestion event (loss, dupack, RTO).
 * Sets wmax, epoch_start, use_cubic, and returns the new ssthresh.
 */
uint32_t cubic_on_loss(struct cubic_data *c, uint32_t current_cwnd,
                       uint64_t now);

/* Return current W_max */
uint32_t cubic_get_wmax(struct cubic_data *c);

/* Set W_max (for idle restart tracking) */
void     cubic_set_wmax(struct cubic_data *c, uint32_t wmax);

/* ── Hybrid slow start (RFC 8312 §3) ──────────────────────────────────── */

/*
 * Update hybrid slow start detection on each ACK during slow start.
 * Monitors ACK spacing for train-length detection and tracks min RTT
 * for delay-based detection.  Returns 1 if slow start should exit.
 *
 * @c       Per-connection CUBIC state
 * @rtt_ms  Current RTT sample (ms)
 * @now_ms  Current timestamp (ms)
 * Returns  1 if hybrid detection signals slow start exit, 0 otherwise.
 */
int      cubic_hystart_update(struct cubic_data *c, uint32_t rtt_ms,
                              uint64_t now_ms);

/*
 * Reset hybrid slow start state.  Called when entering a new slow start
 * phase (connection init, RTO recovery, idle restart).
 */
void     cubic_hystart_reset(struct cubic_data *c);

#endif /* TCP_CUBIC_H */
