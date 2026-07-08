/*
 * tcp_bbr3.c — BBRv3 congestion control with ECN support
 *
 * BBRv3 extends BBRv1 with Explicit Congestion Notification (ECN) awareness.
 * It differentiates between congestion signal types:
 *   - ECN CE marks  → mild congestion, gentle 25% BW estimate backoff
 *   - Packet loss   → strong congestion, full 50% BW estimate backoff
 *
 * This allows BBRv3 to maintain higher throughput under light congestion
 * by responding proportionally to the severity of the congestion signal.
 *
 * State machine (same as BBRv1):
 *   STARTUP   — exponential growth to find bottleneck BW
 *   DRAIN     — drain queue built during STARTUP
 *   PROBE_BW  — steady state, cycle pacing gain to probe for more BW
 *   PROBE_RTT — periodically measure min RTT (cwnd -> 4 segments)
 *
 * ECN integration:
 *   bbr3_on_ecn_ce()   — called when TCP receives an ECN CE mark
 *   bbr3_on_ecn_setup() — called when ECN handshake completes
 *   ecn_ewma            — EWMA of CE marks per round (smoothed signal)
 *   Differentiated BW backoff: ECN mild (0.75x), loss strong (0.50x)
 *
 * References:
 *   - draft-cardwell-iccrg-bbr: BBR Congestion Control
 *   - RFC 3168: The Addition of Explicit Congestion Notification to TCP
 *   - RFC 7560: ECN + TCP Control Bits
 *   - RFC 8511: TCP Alternative Backoff with ECN (ABE)
 */

#include "tcp_bbr3.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/* UINT32_MAX for min_rtt initialisation */
#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

/* ── BBRv3 constants ────────────────────────────────────────────────── */

/* BBR state machine */
#define BBR3_STARTUP    0
#define BBR3_DRAIN      1
#define BBR3_PROBE_BW   2
#define BBR3_PROBE_RTT  3

/* Gains (fixed-point with 8 fractional bits, i.e. gain = value / 256) */
#define BBR3_GAIN_SCALE         8
#define BBR3_UNIT               (1U << BBR3_GAIN_SCALE)   /* 256 = 1.0 */

/* STARTUP gain: 2/ln(2) ≈ 2.885 — doubles delivery rate each round */
#define BBR3_STARTUP_GAIN       (BBR3_UNIT * 2885 / 1000)

/* DRAIN pacing gain: reciprocal of STARTUP gain ≈ 0.347 */
#define BBR3_DRAIN_GAIN_PACING  (BBR3_UNIT * 347 / 1000)

/* PROBE_BW gains */
#define BBR3_PROBE_BW_GAIN      (5 * BBR3_UNIT / 4)       /* 1.25 */
#define BBR3_PROBE_BW_GAIN_LOW  (3 * BBR3_UNIT / 4)       /* 0.75 */
#define BBR3_PROBE_RTT_GAIN     (BBR3_UNIT)               /* 1.0  */

/* How many rounds before checking if BW has plateaued in STARTUP */
#define BBR3_STARTUP_ROUNDS     3

/* PROBE_BW cycle: 8 phases */
#define BBR3_PROBE_BW_CYCLE_LEN 8

/* PROBE_BW gain table */
static const uint32_t bbr3_probe_bw_gains[BBR3_PROBE_BW_CYCLE_LEN] = {
	BBR3_PROBE_BW_GAIN,       /* ProbeUp:   1.25 */
	BBR3_PROBE_BW_GAIN_LOW,   /* ProbeDown: 0.75 */
	BBR3_UNIT,                 /* Cruise:    1.0  */
	BBR3_UNIT,
	BBR3_UNIT,
	BBR3_UNIT,
	BBR3_UNIT,
	BBR3_UNIT,
};

/* How often (in seconds) to enter PROBE_RTT to refresh min_rtt */
#define BBR3_PROBE_RTT_INTERVAL       10
#define BBR3_PROBE_RTT_DURATION_TICKS 20

/* Minimum and maximum cwnd in segments */
#define BBR3_MIN_CWND            4
#define BBR3_MAX_CWND            512

/* EWMA alpha for ECN and loss smoothing (1/8 in 8.8 fixed-point = 32/256) */
#define BBR3_EWMA_ALPHA          32    /* 32/256 = 1/8 */

/* Whether to enable BBRv3 debug logging */
/* #define BBR3_DEBUG */

/* ── Forward declarations ───────────────────────────────────────────── */

static void bbr3_update_bw_filter(struct bbr3_data *b);

/* ── BBRv3 initialisation ───────────────────────────────────────────── */

void bbr3_init(struct bbr3_data *b)
{
	if (!b) return;
	memset(b, 0, sizeof(*b));
	b->state = BBR3_STARTUP;
	b->startup_rounds = 0;
	b->min_rtt = UINT32_MAX;
	b->probe_bw_phase = 0;
	b->pacing_rate = 100;         /* initial 100 bytes/tick */
	b->target_cwnd = 32;          /* 32 segments initial */
	b->bw_initialized = 0;
	b->packet_conservation = 0;
	b->probe_rtt_round_done = 0;
	b->probe_rtt_done_stamp = 0;
	b->probe_bw_last_round = 0;

	/* RTprop estimation */
	b->rtprop_round_min = UINT32_MAX;
	b->rtprop_min_rtt = 0;
	b->rtprop_idx = 0;
	b->rtprop_initialized = 0;

	/* Max BW filter */
	b->max_bw_idx = 0;
	b->max_bw = 0;

	/* BBRv3 ECN defaults */
	b->ecn_ce_threshold = BBR3_ECN_CE_THRESHOLD;
	b->ecn_marks = 0;
	b->ecn_backoff_active = 0;
	b->ecn_round_ce = 0;
	b->ecn_ewma = 0;

	/* Loss tracking */
	b->loss_round = 0;
	b->loss_ewma = 0;
	b->loss_recovery_rounds = 0;
}

/* ── Delivery rate sampling ──────────────────────────────────────────── */

static void bbr3_sample_bw(struct bbr3_data *b, uint32_t acked_bytes,
                            uint64_t now)
{
	if (acked_bytes == 0)
		return;

	b->delivered += acked_bytes;
	b->delivered_tick = now;

	if (b->ack_epoch_tick_start == 0) {
		b->ack_epoch_tick_start = now;
		b->ack_epoch_bytes = 0;
	}

	b->ack_epoch_bytes += acked_bytes;

	uint64_t epoch_delta = now - b->ack_epoch_tick_start;
	if (epoch_delta > 5) {  /* minimum 50ms epoch for stable sample */
		uint32_t bw_sample = (uint32_t)((uint64_t)acked_bytes * 100
		                                / epoch_delta);
		if (!b->bw_initialized) {
			b->bw = bw_sample;
			b->bw_initialized = 1;
		} else {
			int32_t diff = (int32_t)bw_sample - (int32_t)b->bw;
			b->bw += (uint32_t)((int32_t)diff / 8);
		}

		b->ack_epoch_bytes = 0;
		b->ack_epoch_tick_start = now;
	}

	if (b->bw > b->bw_hi)
		b->bw_hi = b->bw;
}

/* ── Round detection ─────────────────────────────────────────────────── */

static void bbr3_update_round(struct bbr3_data *b, uint32_t cwnd_segments,
                               uint64_t now)
{
	uint32_t delivered_this_round = b->delivered - b->round_delivered;
	uint32_t round_target = cwnd_segments * 1400;  /* approximate MSS bytes */

	if (b->round_delivered == 0) {
		b->round_delivered = b->delivered;
		b->round_start = now;
		return;
	}

	if (delivered_this_round >= round_target) {
		/* New round detected — update ECN/loss EWMA before reset */
		b->ecn_ewma = (b->ecn_ewma * (256 - BBR3_EWMA_ALPHA)
		               + b->ecn_round_ce * BBR3_EWMA_ALPHA) / 256;
		b->loss_ewma = (b->loss_ewma * (256 - BBR3_EWMA_ALPHA)
		                + b->loss_round * BBR3_EWMA_ALPHA) / 256;

		b->round_count++;
		b->round_delivered = b->delivered;
		b->round_start = now;

		/* Reset round-local counters */
		b->ecn_round_ce = 0;
		b->loss_round = 0;

		/* Decrement loss recovery countdown */
		if (b->loss_recovery_rounds > 0)
			b->loss_recovery_rounds--;

#ifdef BBR3_DEBUG
		kprintf("[BBRv3] Round %d complete (cwnd=%u, bw=%u, "
		        "ecn_ewma=%u, loss_ewma=%u)\n",
		        b->round_count, cwnd_segments, b->bw,
		        b->ecn_ewma, b->loss_ewma);
#endif

		bbr3_update_bw_filter(b);
	}
}

/* ── Max BW filter (windowed max, same as BBRv1 §4.1) ────────────────── */

static void bbr3_update_bw_filter(struct bbr3_data *b)
{
	if (!b) return;

	b->max_bw_filter[b->max_bw_idx] = b->bw;
	b->max_bw_idx = (b->max_bw_idx + 1) % BBR3_MAX_BW_FILTER_LEN;

	uint32_t new_max = 0;
	for (int i = 0; i < BBR3_MAX_BW_FILTER_LEN; i++) {
		if (b->max_bw_filter[i] > new_max)
			new_max = b->max_bw_filter[i];
	}
	b->max_bw = new_max;
}

/* ── RTprop estimation (windowed min RTT, same as BBRv1 §4.3) ────────── */

static void bbr3_update_rtprop(struct bbr3_data *b)
{
	uint32_t sample;

	if (!b)
		return;

	sample = b->rtprop_round_min;
	if (sample == 0 || sample == UINT32_MAX) {
		if (b->min_rtt > 0 && b->min_rtt != UINT32_MAX)
			sample = b->min_rtt;
		else
			return;
	}

	b->rtprop_filter[b->rtprop_idx] = sample;
	b->rtprop_idx = (b->rtprop_idx + 1) % BBR3_RTPROP_FILTER_LEN;
	b->rtprop_initialized = 1;

	{
		uint32_t new_min = UINT32_MAX;
		for (int i = 0; i < BBR3_RTPROP_FILTER_LEN; i++) {
			uint32_t val = b->rtprop_filter[i];
			if (val > 0 && val < new_min)
				new_min = val;
		}
		b->rtprop_min_rtt = (new_min == UINT32_MAX) ? 0 : new_min;
	}
}

/* ── ECN-aware bandwidth selection ───────────────────────────────────── */

/*
 * BBRv3 ECN-aware BW estimation.
 *
 * Returns the bandwidth value to use for pacing rate calculation,
 * applying ECN-based backoff to the windowed max BW when congestion
 * is detected via ECN CE marks.
 *
 * When ECN CE marks exceed the threshold, the effective BW is reduced
 * by BBR3_ECN_BW_FACTOR (0.75 = 25% reduction).  This is gentler than
 * the loss-based reduction (0.50 = 50% reduction), allowing BBRv3 to
 * maintain higher throughput under mild congestion.
 *
 * If both ECN and loss signals are present simultaneously, the stronger
 * (loss-based) backoff takes precedence.
 */
static uint32_t bbr3_select_bw_for_pacing(struct bbr3_data *b)
{
	uint32_t bw_for_pacing;
	uint32_t effective_bw;
	int      apply_ecn_backoff = 0;

	/* Base BW from windowed max filter or fallback */
	if (b->max_bw > 0)
		bw_for_pacing = b->max_bw;
	else if (b->bw > 0)
		bw_for_pacing = b->bw;
	else
		bw_for_pacing = 100;

	effective_bw = bw_for_pacing;

	/*
	 * Apply ECN-based backoff if CE marks above threshold.
	 * BBRv3 uses a gentler backoff for ECN than for loss,
	 * reflecting the fact that ECN signals mild congestion.
	 */
	if (b->ecn_marks >= b->ecn_ce_threshold) {
		apply_ecn_backoff = 1;
		effective_bw = (effective_bw * BBR3_ECN_BW_FACTOR)
		               >> BBR3_GAIN_SCALE;
		b->ecn_backoff_active = 1;

#ifdef BBR3_DEBUG
		kprintf("[BBRv3] ECN backoff: bw=%u -> %u (marks=%u)\n",
		        bw_for_pacing, effective_bw, b->ecn_marks);
#endif
	}

	/*
	 * If loss EWMA indicates persistent loss, apply the stronger
	 * loss-based backoff.  This overrides the ECN backoff because
	 * loss is a stronger congestion signal.
	 */
	if (b->loss_ewma > 0) {
		effective_bw = (bw_for_pacing * BBR3_LOSS_BW_FACTOR)
		               >> BBR3_GAIN_SCALE;
		b->ecn_backoff_active = 0;  /* loss overrides ECN */

#ifdef BBR3_DEBUG
		kprintf("[BBRv3] Loss backoff: bw=%u -> %u (ewma=%u)\n",
		        bw_for_pacing, effective_bw, b->loss_ewma);
#endif
	}

	if (apply_ecn_backoff && b->ecn_backoff_active && b->loss_ewma == 0) {
		/* ECN backoff is active */
	} else if (!apply_ecn_backoff) {
		b->ecn_backoff_active = 0;
	}

	/* Ensure minimum pacing */
	if (effective_bw < 10)
		effective_bw = 10;

	return effective_bw;
}

/* ── Pacing rate estimation (BBRv3 with ECN-aware BW) ────────────────── */

void bbr3_update_pacing_rate(struct bbr3_data *b)
{
	if (!b) return;

	uint32_t gain = BBR3_UNIT;

	switch (b->state) {
	case BBR3_STARTUP:
		gain = BBR3_STARTUP_GAIN;
		break;
	case BBR3_DRAIN:
		gain = BBR3_DRAIN_GAIN_PACING;
		break;
	case BBR3_PROBE_BW:
		gain = bbr3_probe_bw_gains[b->probe_bw_phase
		                            % BBR3_PROBE_BW_CYCLE_LEN];
		break;
	case BBR3_PROBE_RTT:
		gain = BBR3_PROBE_RTT_GAIN;
		break;
	default:
		break;
	}

	/*
	 * During loss recovery, force gain to 1.0 regardless of state.
	 * This suppresses bandwidth probing while the connection drains
	 * queues and stabilises after a congestion event.  Without this,
	 * BBR would immediately resume probing (gain > 1) after one round,
	 * potentially causing another loss.
	 */
	if (b->loss_recovery_rounds > 0)
		gain = BBR3_UNIT;

	/*
	 * BBRv3: use ECN-aware BW selection instead of raw max_bw.
	 * This is the key difference from BBRv1 — the bandwidth
	 * estimate used for pacing is reduced in response to ECN
	 * congestion signals.
	 */
	uint32_t bw_for_pacing = bbr3_select_bw_for_pacing(b);

	uint32_t rate = (uint32_t)(((uint64_t)bw_for_pacing * gain)
	                           >> BBR3_GAIN_SCALE);
	if (rate < 10) rate = 10;
	b->pacing_rate = rate;
}

/* ── Main ACK processing ─────────────────────────────────────────────── */

void bbr3_on_ack(struct bbr3_data *b, uint32_t acked_bytes,
                  uint32_t cwnd_segments, uint32_t rtt_ticks, uint64_t now)
{
	if (!b) return;

	/* Update min RTT */
	if (rtt_ticks > 0 && rtt_ticks < b->min_rtt) {
		b->min_rtt = rtt_ticks;
		b->min_rtt_stamp = now;
	}

	/* Track per-round minimum RTT for RTprop */
	if (rtt_ticks > 0 && rtt_ticks < b->rtprop_round_min)
		b->rtprop_round_min = rtt_ticks;

	/* Sample delivery rate */
	bbr3_sample_bw(b, acked_bytes, now);

	/* Update round tracking */
	{
		uint16_t prev_round_count = b->round_count;
		bbr3_update_round(b, cwnd_segments, now);
		if (b->round_count != prev_round_count) {
			bbr3_update_rtprop(b);
			b->rtprop_round_min = UINT32_MAX;

			/* At round boundary, re-evaluate ECN backoff */
			if (b->ecn_backoff_active) {
				b->ecn_marks = 0;
				if (b->ecn_ewma == 0)
					b->ecn_backoff_active = 0;
			}
		}
	}

	/* ── STARTUP state ─────────────────────────────────────────────── */
	if (b->state == BBR3_STARTUP) {
		if (b->bw_hi == 0)
			b->bw_hi = b->bw;

		if (b->bw <= b->bw_hi) {
			b->startup_rounds++;
		} else {
			b->bw_hi = b->bw;
			b->startup_rounds = 0;
		}

		if (b->startup_rounds >= BBR3_STARTUP_ROUNDS) {
			b->state = BBR3_DRAIN;
#ifdef BBR3_DEBUG
			kprintf("[BBRv3] STARTUP -> DRAIN (BW=%u bytes/tick)\n",
			        b->bw);
#endif
		}
	}

	/* ── DRAIN state ───────────────────────────────────────────────── */
	if (b->state == BBR3_DRAIN) {
		uint32_t rtt_est = (b->rtprop_min_rtt > 0)
		                   ? b->rtprop_min_rtt : b->min_rtt;
		if (rtt_est > 0 && rtt_est != UINT32_MAX && b->bw > 0) {
			uint32_t bdp_segments = (b->bw * rtt_est) / 1400;
			if (bdp_segments < BBR3_MIN_CWND)
				bdp_segments = BBR3_MIN_CWND;
			b->target_cwnd = bdp_segments;
			if (cwnd_segments <= bdp_segments + 4) {
				b->state = BBR3_PROBE_BW;
				b->probe_bw_phase = 0;
				b->probe_bw_last_round = b->round_count;
#ifdef BBR3_DEBUG
				kprintf("[BBRv3] DRAIN -> PROBE_BW (bdp=%u segs)\n",
				        bdp_segments);
#endif
			}
		}
	}

	/* ── PROBE_BW state ────────────────────────────────────────────── */
	if (b->state == BBR3_PROBE_BW) {
		if (b->round_count != b->probe_bw_last_round) {
			b->probe_bw_last_round = b->round_count;

			/*
			 * During loss recovery, pause the probe phase
			 * cycle.  We still track the round boundary
			 * (probe_bw_last_round updated above) but do
			 * not advance to the next gain phase.  When
			 * recovery ends, probing resumes from the
			 * current phase, avoiding a second probe burst
			 * that could cause further loss.
			 */
			if (b->loss_recovery_rounds == 0) {
				b->probe_bw_phase = (uint8_t)((b->probe_bw_phase + 1)
				                     % BBR3_PROBE_BW_CYCLE_LEN);

#ifdef BBR3_DEBUG
				kprintf("[BBRv3] PROBE_BW phase %d (gain=%u/256)\n",
				        b->probe_bw_phase,
				        bbr3_probe_bw_gains[b->probe_bw_phase]);
#endif
			}
		}

		/* Check if it's time for PROBE_RTT */
		if (b->min_rtt_stamp > 0 &&
		    (now - b->min_rtt_stamp) >
		     (uint64_t)BBR3_PROBE_RTT_INTERVAL * 100) {
			b->state = BBR3_PROBE_RTT;
			b->probe_rtt_done_stamp = now
			                          + BBR3_PROBE_RTT_DURATION_TICKS;
			b->probe_rtt_round_done = 0;
			b->target_cwnd = BBR3_MIN_CWND;
#ifdef BBR3_DEBUG
			kprintf("[BBRv3] PROBE_BW -> PROBE_RTT\n");
#endif
		}
	}

	/* ── PROBE_RTT state ───────────────────────────────────────────── */
	if (b->state == BBR3_PROBE_RTT) {
		if (now >= b->probe_rtt_done_stamp || b->probe_rtt_round_done) {
			b->state = BBR3_PROBE_BW;
			b->probe_bw_phase = 0;
			b->probe_bw_last_round = b->round_count;
			b->min_rtt_stamp = now;
#ifdef BBR3_DEBUG
			kprintf("[BBRv3] PROBE_RTT done, returning to PROBE_BW\n");
#endif
		}
	}

	/* ── Compute pacing rate (ECN-aware) ────────────────────────────── */
	bbr3_update_pacing_rate(b);

	/* ── Compute target cwnd with per-state gains ──────────────────── */
	if (b->state != BBR3_PROBE_RTT) {
		uint32_t rtt_est = (b->rtprop_min_rtt > 0)
		                   ? b->rtprop_min_rtt : b->min_rtt;
		if (rtt_est > 0 && rtt_est != UINT32_MAX && b->bw > 0) {
			uint64_t bdp_bytes = (uint64_t)b->bw * rtt_est;
			uint32_t bdp_segments = (uint32_t)(bdp_bytes / 1400);

			uint32_t cwnd_gain;
			switch (b->state) {
			case BBR3_STARTUP:
				cwnd_gain = BBR3_STARTUP_GAIN;
				break;
			case BBR3_DRAIN:
				cwnd_gain = 2 * BBR3_UNIT;
				break;
			case BBR3_PROBE_BW:
				cwnd_gain = 2 * BBR3_UNIT;
				break;
			default:
				cwnd_gain = BBR3_UNIT;
				break;
			}

			uint32_t target = (uint32_t)(
			    ((uint64_t)bdp_segments * cwnd_gain)
			    >> BBR3_GAIN_SCALE);
			if (target < BBR3_MIN_CWND) target = BBR3_MIN_CWND;
			if (target > BBR3_MAX_CWND) target = BBR3_MAX_CWND;
			b->target_cwnd = target;
		} else {
			b->target_cwnd = 32;
		}
	}
}

/* ── ECN CE mark processing ──────────────────────────────────────────── */

/*
 * Called when an ECN CE (Congestion Experienced) mark is received.
 *
 * BBRv3's handling of ECN is the key differentiator from BBRv1:
 * Instead of treating all congestion signals equally, ECN CE marks
 * trigger a gentler response than packet loss.
 *
 * The CE mark is counted and when the threshold is reached, the
 * bandwidth estimate used for pacing is reduced by 25% (via
 * bbr3_select_bw_for_pacing()).  This is much gentler than the
 * 50% reduction on packet loss, allowing BBRv3 to maintain higher
 * throughput under mild congestion.
 */
void bbr3_on_ecn_ce(struct bbr3_data *b, uint32_t rtt_ticks)
{
	if (!b) return;

	b->ecn_marks++;
	b->ecn_round_ce++;

#ifdef BBR3_DEBUG
	kprintf("[BBRv3] ECN CE: marks=%u, round_ce=%u, threshold=%u\n",
	        b->ecn_marks, b->ecn_round_ce, b->ecn_ce_threshold);
#endif

	/*
	 * When CE marks exceed the threshold, force a pacing rate
	 * update so the ECN backoff takes effect immediately rather
	 * than waiting for the next round boundary.
	 */
	if (b->ecn_marks >= b->ecn_ce_threshold) {
		bbr3_update_pacing_rate(b);

		/*
		 * If still in STARTUP, transition to DRAIN as the ECN
		 * marks indicate the pipe is getting full.
		 */
		if (b->state == BBR3_STARTUP) {
			b->state = BBR3_DRAIN;
#ifdef BBR3_DEBUG
			kprintf("[BBRv3] ECN CE in STARTUP -> DRAIN\n");
#endif
		}
	}
}

/* ── ECN setup notification ──────────────────────────────────────────── */

/*
 * Called when the TCP ECN handshake completes successfully.
 * The connection has negotiated ECN support (both sides set
 * ECE and CWR flags during SYN/SYN-ACK).
 *
 * BBRv3 records that ECN is active so it can respond to CE marks.
 * Currently this is primarily informational — the module responds
 * to CE marks regardless of handshake state.
 */
void bbr3_on_ecn_setup(struct bbr3_data *b)
{
	if (!b) return;

	b->ecn_ce_threshold = BBR3_ECN_CE_THRESHOLD;

#ifdef BBR3_DEBUG
	kprintf("[BBRv3] ECN setup complete — CE threshold=%u\n",
	        b->ecn_ce_threshold);
#endif
}

/* ── Packet loss / congestion event ──────────────────────────────────── */

/*
 * Called on packet loss or retransmit.
 *
 * BBRv3 differentiates loss from ECN:
 * - Loss is a strong congestion signal → full backoff (0.50x BW)
 * - ECN is a mild congestion signal → gentle backoff (0.75x BW)
 *
 * On loss in STARTUP, immediately transition to DRAIN.
 */
void bbr3_on_loss(struct bbr3_data *b)
{
	if (!b) return;

	b->loss_round++;
	b->packet_conservation = 1;

	/*
	 * Enter loss recovery for BBR3_LOSS_RECOVERY_ROUNDS rounds.
	 * During recovery, pacing gain is forced to 1.0 (no probing,
	 * see bbr3_update_pacing_rate) and the PROBE_BW phase cycle
	 * is paused.  This gives the connection time to drain queues
	 * and stabilise after the loss event.
	 */
	b->loss_recovery_rounds = BBR3_LOSS_RECOVERY_ROUNDS;

	/* Force pacing rate update with loss backoff */
	bbr3_update_pacing_rate(b);

	if (b->state == BBR3_STARTUP) {
		b->state = BBR3_DRAIN;
#ifdef BBR3_DEBUG
		kprintf("[BBRv3] Loss in STARTUP -> DRAIN\n");
#endif
	}
}

/* ── Public accessors ────────────────────────────────────────────────── */

uint32_t bbr3_get_cwnd(struct bbr3_data *b, uint32_t current_cwnd)
{
	if (!b) return current_cwnd;

	uint32_t target = b->target_cwnd;
	if (target < BBR3_MIN_CWND) target = BBR3_MIN_CWND;

	if (b->packet_conservation) {
		if (target > current_cwnd)
			target = current_cwnd;
		b->packet_conservation = 0;
	}

	return target;
}

uint32_t bbr3_get_pacing_rate(struct bbr3_data *b)
{
	if (!b) return 0;
	return b->pacing_rate;
}

const char *bbr3_state_str(struct bbr3_data *b)
{
	if (!b) return "?";
	switch (b->state) {
	case BBR3_STARTUP:   return "STARTUP";
	case BBR3_DRAIN:     return "DRAIN";
	case BBR3_PROBE_BW:  return "PROBE_BW";
	case BBR3_PROBE_RTT: return "PROBE_RTT";
	default:             return "UNKNOWN";
	}
}

void bbr3_dump(struct bbr3_data *b)
{
	if (!b) return;
	kprintf("[BBRv3] state=%s bw=%u bw_hi=%u max_bw=%u min_rtt=%u "
	        "rtprop=%u pacer=%u tgt_cwnd=%u rounds=%u phase=%u "
	        "ecn_marks=%u ecn_ewma=%u loss_ewma=%u loss_rec=%u "
	        "ecn_backoff=%d\n",
	        bbr3_state_str(b), b->bw, b->bw_hi, b->max_bw, b->min_rtt,
	        b->rtprop_min_rtt,
	        b->pacing_rate, b->target_cwnd, b->round_count,
	        b->probe_bw_phase,
	        b->ecn_marks, b->ecn_ewma, b->loss_ewma,
	        b->loss_recovery_rounds,
	        b->ecn_backoff_active);
}

int bbr3_is_active(struct bbr3_data *b)
{
	return b && b->rtprop_initialized && b->bw_initialized;
}

/* ── Module infrastructure ──────────────────────────────────────────── */

#include "module.h"

static int __init bbr3_module_init(void)
{
	kprintf("[OK] BBRv3 — Congestion control with ECN support\n");
	return 0;
}

module_init(bbr3_module_init);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("BBRv3 congestion control with ECN support — loadable kernel module");
