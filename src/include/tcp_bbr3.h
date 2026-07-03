#ifndef TCP_BBR3_H
#define TCP_BBR3_H

#include "types.h"

/*
 * BBRv3 congestion control state — ECN-aware extension of BBRv1.
 *
 * BBRv3 adds differentiated congestion signalling:
 *   - ECN CE marks  → mild congestion, gentle rate backoff
 *   - Packet loss   → strong congestion, full model reset
 *
 * This allows BBRv3 to maintain higher throughput under light
 * congestion by responding proportionally to the congestion signal.
 */

/* Max BW filter length in rounds (windowed max filter) */
#define BBR3_MAX_BW_FILTER_LEN  10

/* RTprop filter length in rounds (windowed min RTT filter) */
#define BBR3_RTPROP_FILTER_LEN  10

/* ECN CE mark threshold — number of CE marks before reacting */
#define BBR3_ECN_CE_THRESHOLD   3

/* ECN scaling factor: how much to reduce BW estimate per CE mark
 * (fixed-point, denominator = 256, so 192 = 0.75 = reduce by 25%) */
#define BBR3_ECN_BW_FACTOR      192   /* 0.75 in 8.8 fixed-point */

/* Loss-based backoff factor: how much to reduce BW estimate on loss
 * (fixed-point, denominator = 256, so 128 = 0.50 = halve) */
#define BBR3_LOSS_BW_FACTOR     128   /* 0.50 in 8.8 fixed-point */

/* ── BBRv3 per-connection state ─────────────────────────────────────── */

struct bbr3_data {
	/* ── Bandwidth estimation ─────────────────────────────────────── */
	uint32_t bw;                    /* smoothed bandwidth (bytes/tick) */
	uint32_t bw_hi;                 /* highest BW seen (for STARTUP plateau) */

	/* Max BW filter (windowed max over BBR3_MAX_BW_FILTER_LEN rounds) */
	uint32_t max_bw_filter[BBR3_MAX_BW_FILTER_LEN];
	int      max_bw_idx;
	uint32_t max_bw;                /* windowed max bandwidth */

	/* ── RTT estimation ───────────────────────────────────────────── */
	uint32_t min_rtt;               /* global min RTT (ticks) */
	uint64_t min_rtt_stamp;         /* tick when min_rtt was last updated */

	/* RTprop estimation (windowed min RTT) */
	uint32_t rtprop_filter[BBR3_RTPROP_FILTER_LEN];
	int      rtprop_idx;
	uint32_t rtprop_min_rtt;
	uint32_t rtprop_round_min;
	int      rtprop_initialized;

	/* ── Round tracking ───────────────────────────────────────────── */
	uint64_t round_start;
	uint16_t round_count;
	uint32_t round_delivered;
	uint32_t delivered;
	uint64_t delivered_tick;

	/* ── State machine ────────────────────────────────────────────── */
	uint8_t  state;                 /* BBR_STARTUP/DRAIN/PROBE_BW/PROBE_RTT */
	uint8_t  probe_bw_phase;        /* phase within PROBE_BW cycle (0-7) */
	uint8_t  startup_rounds;

	uint16_t probe_bw_last_round;

	/* ── Pacing & cwnd ────────────────────────────────────────────── */
	uint32_t pacing_rate;           /* bytes per tick */
	uint32_t target_cwnd;           /* target congestion window (segments) */

	/* ── PROBE_RTT timing ─────────────────────────────────────────── */
	uint64_t probe_rtt_done_stamp;
	uint8_t  probe_rtt_round_done;
	uint8_t  packet_conservation;

	/* ── ACK rate sampling ────────────────────────────────────────── */
	uint32_t ack_epoch_bytes;
	uint64_t ack_epoch_tick_start;
	int      bw_initialized;

	/* ── BBRv3 ECN-specific fields ────────────────────────────────── */

	/*
	 * ECN CE mark tracking.
	 * ecn_marks:      cumulative CE marks received this interval.
	 * ecn_ce_threshold:  CE marks before triggering ECN backoff.
	 * ecn_backoff_active: 1 if ECN-induced backoff is in effect.
	 * ecn_round_ce:    CE marks seen in the current round.
	 * ecn_ewma:        EWMA of CE mark rate (smoothed signal).
	 */
	uint32_t ecn_marks;
	uint32_t ecn_ce_threshold;
	int      ecn_backoff_active;
	uint32_t ecn_round_ce;
	uint32_t ecn_ewma;              /* EWMA of CE marks per round (scaled) */

	/*
	 * Loss tracking.
	 * loss_round:      loss events seen in the current round.
	 * loss_ewma:       EWMA of loss events per round (smoothed).
	 */
	uint32_t loss_round;
	uint32_t loss_ewma;
};

/* ── BBRv3 public API ────────────────────────────────────────────────── */

/* Allocate and initialise BBRv3 state */
void     bbr3_init(struct bbr3_data *b);

/* Called on every ACK with ACKed bytes count and current RTT */
void     bbr3_on_ack(struct bbr3_data *b, uint32_t acked_bytes,
                      uint32_t cwnd_segments, uint32_t rtt_ticks,
                      uint64_t now);

/* Called on packet loss / retransmit */
void     bbr3_on_loss(struct bbr3_data *b);

/* Called when ECN CE (Congestion Experienced) mark is received */
void     bbr3_on_ecn_ce(struct bbr3_data *b, uint32_t rtt_ticks);

/* Called when ECN ECT setup is confirmed (ECN handshake complete) */
void     bbr3_on_ecn_setup(struct bbr3_data *b);

/* Return target congestion window (segments) */
uint32_t bbr3_get_cwnd(struct bbr3_data *b, uint32_t current_cwnd);

/* Return pacing rate (bytes per tick) */
uint32_t bbr3_get_pacing_rate(struct bbr3_data *b);

/* Update pacing rate using BBRv3 formula with ECN-aware BW */
void     bbr3_update_pacing_rate(struct bbr3_data *b);

/* Return BBRv3 state as string */
const char *bbr3_state_str(struct bbr3_data *b);

/* Dump BBRv3 state to kernel log */
void     bbr3_dump(struct bbr3_data *b);

/* Check whether BBRv3 is actively running with estimates */
int      bbr3_is_active(struct bbr3_data *b);

#endif /* TCP_BBR3_H */
