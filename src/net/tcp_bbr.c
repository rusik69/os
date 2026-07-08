/*
 * tcp_bbr.c — BBR (Bottleneck Bandwidth and Round-trip propagation time)
 * congestion control — model-based, not loss-based
 *
 * BBR estimates the bottleneck bandwidth and round-trip propagation delay,
 * and paces sending at the estimated bandwidth, keeping the network pipe
 * just full enough without building queues.
 *
 * References:
 *   - BBR Congestion Control (RFC 8967 / draft-cardwell-iccrg-bbr)
 *   - Neal Cardwell et al., "BBR: Congestion-Based Congestion Control"
 *     ACM Queue, 2016
 *
 * State machine:
 *   STARTUP   — exponential growth to find bottleneck BW (8% per RTT)
 *   DRAIN     — drain queue built during STARTUP (pacing gain < 1)
 *   PROBE_BW  — steady state, cycle pacing gain to probe for more BW
 *   PROBE_RTT — periodically measure min RTT (cwnd -> 4 segments)
 *
 * Integration:
 *   bbr_on_ack()    — called on each ACK (data delivery event)
 *   bbr_on_loss()   — called on retransmit / packet loss
 *   bbr_get_pacing_rate() — returns rate in bytes/tick for pacing
 *   bbr_get_cwnd()  — returns target congestion window in segments
 *
 * NOTE: This is a practical BBR implementation for a hobby OS.
 * It does NOT implement the full BBRv3 with ECN or ACK aggregation
 * heuristics, but captures the core algorithm faithfully.
 */

#include "tcp_bbr.h"
#include "types.h"
#include "printf.h"
#include "string.h"
#include "timer.h"

/* UINT32_MAX for min_rtt initialisation (not in types.h) */
#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif

/* ── BBR constants ──────────────────────────────────────────────────── */

/* BBR state machine */
#define BBR_STARTUP    0
#define BBR_DRAIN      1
#define BBR_PROBE_BW   2
#define BBR_PROBE_RTT  3

/* Gains (fixed-point with 8 fractional bits, i.e. gain = value / 256) */
#define BBR_GAIN_SCALE         8
#define BBR_UNIT               (1U << BBR_GAIN_SCALE)   /* 256 = 1.0 */

/*
 * STARTUP gain: 2/ln(2) ≈ 2.885 (RFC 8967 / BBR v1 paper)
 * Each round, pacing at 2.885x the estimated BW doubles the delivery rate.
 */
#define BBR_STARTUP_GAIN       (BBR_UNIT * 2885 / 1000) /* 2.885 : 2x growth per RTT */

/*
 * DRAIN pacing gain: reciprocal of STARTUP gain ≈ 0.347.
 * Drains the queue built during STARTUP in approximately one RTT.
 */
#define BBR_DRAIN_GAIN_PACING  (BBR_UNIT * 347 / 1000)  /* 0.347 : drain queue */

/* Legacy DRAIN comment — pacing gain handles drain */
#define BBR_PROBE_BW_GAIN      (5 * BBR_UNIT / 4)       /* 1.25 : probe for more BW */
#define BBR_PROBE_BW_GAIN_LOW  (3 * BBR_UNIT / 4)       /* 0.75 : drain after probe */
#define BBR_PROBE_RTT_GAIN     (BBR_UNIT)               /* 1.0  : neutral */

/* How many rounds (RTTs) before checking if BW has plateaued in STARTUP */
#define BBR_STARTUP_ROUNDS     3

/* PROBE_BW cycle: 8 phases, gain=1.25 for first, 0.75 for next, 1.0 for rest */
#define BBR_PROBE_BW_CYCLE_LEN 8

/*
 * BBR v1 PROBE_BW gain table — one entry per phase of the 8-round cycle.
 *   phase 0 (ProbeUp):    gain=1.25  — probe for more bandwidth
 *   phase 1 (ProbeDown):  gain=0.75  — drain queue from probe
 *   phases 2-7 (Cruise):  gain=1.0   — neutral cruising
 */
static const uint32_t bbr_probe_bw_gains[BBR_PROBE_BW_CYCLE_LEN] = {
    BBR_PROBE_BW_GAIN,       /* ProbeUp:   1.25  — send faster to fill available bw */
    BBR_PROBE_BW_GAIN_LOW,   /* ProbeDown: 0.75  — drain any queue built up */
    BBR_UNIT,                 /* Cruise:    1.0   */
    BBR_UNIT,                 /* Cruise:    1.0   */
    BBR_UNIT,                 /* Cruise:    1.0   */
    BBR_UNIT,                 /* Cruise:    1.0   */
    BBR_UNIT,                 /* Cruise:    1.0   */
    BBR_UNIT,                 /* Cruise:    1.0   */
};

/* How often (in seconds) to enter PROBE_RTT to refresh min_rtt */
#define BBR_PROBE_RTT_INTERVAL    10    /* seconds */
#define BBR_PROBE_RTT_DURATION     0.2  /* 200 ms in ticks (20 ticks @ 100Hz) -- use int */
#define BBR_PROBE_RTT_DURATION_TICKS 20

/* Minimum cwnd in segments (BBR never goes below this) */
#define BBR_MIN_CWND            4

/* Maximum cwnd cap in segments */
#define BBR_MAX_CWND            512

/* Whether to enable BBR debug logging */
/* #define BBR_DEBUG */

/* ── BBR initialization ─────────────────────────────────────────────── */

void bbr_init(struct bbr_data *b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
    b->state = BBR_STARTUP;
    b->startup_rounds = 0;
    b->min_rtt = UINT32_MAX;
    b->probe_bw_phase = 0;
    b->pacing_rate = 100;         /* initial 100 bytes/tick (10 KB/s @ 100Hz) */
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
    /* rtprop_filter[] zeroed by memset — zero entries are treated as empty */

    /* Max BW filter starts empty — will populate as rounds complete */
    b->max_bw_idx = 0;
    b->max_bw = 0;
}

/* ── Delivery rate sampling ──────────────────────────────────────────── */

/*
 * Called on every ACK with the number of bytes newly ACKed.
 * Updates the bandwidth sample.
 */
static void bbr_sample_bw(struct bbr_data *b, uint32_t acked_bytes, uint64_t now)
{
    if (acked_bytes == 0)
        return;

    /* Track cumulative delivered bytes for round detection */
    b->delivered += acked_bytes;
    b->delivered_tick = now;

    /* Rate sampling: bytes ACKed per tick */
    if (b->ack_epoch_tick_start == 0) {
        b->ack_epoch_tick_start = now;
        b->ack_epoch_bytes = 0;
    }

    b->ack_epoch_bytes += acked_bytes;

    uint64_t epoch_delta = now - b->ack_epoch_tick_start;
    if (epoch_delta > 5) {  /* minimum 50ms epoch for stable sample */
        uint32_t bw_sample = (uint32_t)((uint64_t)acked_bytes * 100 / epoch_delta);
        /* Smooth the BW estimate (EWMA, alpha = 1/8) */
        if (!b->bw_initialized) {
            b->bw = bw_sample;
            b->bw_initialized = 1;
        } else {
            /* Alpha = 1/8 */
            int32_t diff = (int32_t)bw_sample - (int32_t)b->bw;
            b->bw += (uint32_t)((int32_t)diff / 8);
        }

        /* Start a new epoch */
        b->ack_epoch_bytes = 0;
        b->ack_epoch_tick_start = now;
    }

    /* Track running max BW */
    if (b->bw > b->bw_hi)
        b->bw_hi = b->bw;
}

/* ── Round detection ─────────────────────────────────────────────────── */

/*
 * Forward declaration for static helper defined below.
 */
static void bbr_update_bw_filter(struct bbr_data *b);

/*
 * A "round" is an interval of time during which one RTT's worth of data
 * has been delivered (i.e., the delivery counter advances by the cwnd).
 * We detect the start of a new round when the delivery counter exceeds
 * the count at the start of the current round + the current cwnd.
 */
static void bbr_update_round(struct bbr_data *b, uint32_t cwnd_segments,
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
        /* New round has started */
        b->round_count++;
        b->round_delivered = b->delivered;
        b->round_start = now;

#ifdef BBR_DEBUG
        kprintf("[BBR] Round %d complete (cwnd=%u, bw=%u bytes/tick)\n",
                b->round_count, cwnd_segments, b->bw);
#endif

        /* Update the windowed max BW filter when a round completes */
        bbr_update_bw_filter(b);
    }
}

/* ── Max BW filter (windowed max, BBR v1 §4.1) ──────────────────────── */

/*
 * Update the windowed max bandwidth filter.
 * Called at the end of each full round.
 *
 * Stores the current BW sample (the EWMA-smoothed bw) into a circular
 * buffer, then recomputes max_bw as the maximum across the window.
 * This gives a robust estimate of the bottleneck bandwidth, immune to
 * transient dips caused by sender-idle periods or ACK aggregation.
 */
static void bbr_update_bw_filter(struct bbr_data *b)
{
    if (!b) return;

    /* Store current BW into the circular filter */
    b->max_bw_filter[b->max_bw_idx] = b->bw;
    b->max_bw_idx = (b->max_bw_idx + 1) % BBR_MAX_BW_FILTER_LEN;

    /* Recompute the windowed max */
    uint32_t new_max = 0;
    for (int i = 0; i < BBR_MAX_BW_FILTER_LEN; i++) {
        if (b->max_bw_filter[i] > new_max)
            new_max = b->max_bw_filter[i];
    }
    b->max_bw = new_max;

#ifdef BBR_DEBUG
    kprintf("[BBR] bw_filter: bw=%u max_bw=%u idx=%d\n",
            b->bw, b->max_bw, b->max_bw_idx);
#endif
}

/* ── RTprop estimation (BBR v1 §4.3) ──────────────────────────────────── */

/*
 * Update the windowed-minimum RTT filter (RTprop estimate).
 * Called at each round completion.
 *
 * Stores the current round's minimum measured RTT as a sample into a
 * circular buffer, then recomputes the windowed minimum across the
 * buffer.  This yields a robust estimate of the propagation delay
 * (RTprop) that is resilient to transient queue-induced RTT spikes.
 *
 * If no valid RTT sample was collected during the round (should not
 * happen in normal operation), we fall back to the global min_rtt.
 */
static void bbr_update_rtprop(struct bbr_data *b)
{
    uint32_t sample;

    if (!b)
        return;

    /* Use the current round's minimum RTT, or fall back to min_rtt */
    sample = b->rtprop_round_min;
    if (sample == 0 || sample == UINT32_MAX) {
        if (b->min_rtt > 0 && b->min_rtt != UINT32_MAX)
            sample = b->min_rtt;
        else
            return;  /* No valid RTT data yet */
    }

    /* Store sample in the ring buffer */
    b->rtprop_filter[b->rtprop_idx] = sample;
    b->rtprop_idx = (b->rtprop_idx + 1) % BBR_RTPROP_FILTER_LEN;
    b->rtprop_initialized = 1;

    /* Recompute windowed minimum — skip zero entries (uninitialized) */
    {
        uint32_t new_min = UINT32_MAX;
        for (int i = 0; i < BBR_RTPROP_FILTER_LEN; i++) {
            uint32_t val = b->rtprop_filter[i];
            if (val > 0 && val < new_min)
                new_min = val;
        }
        b->rtprop_min_rtt = (new_min == UINT32_MAX) ? 0 : new_min;
    }

#ifdef BBR_DEBUG
    kprintf("[BBR] rtprop: round_min=%u win_min=%u idx=%d\n",
            sample, b->rtprop_min_rtt, b->rtprop_idx);
#endif
}

/* ── Pacing rate estimation (BBR v1 §4.2) ──────────────────────────── */

/*
 * Update the pacing rate using the BBR v1 formula:
 *
 *   pacing_rate = max_bw_filter × pacing_gain / BBR_UNIT
 *
 * The gain is selected by current state:
 *   STARTUP   → BBR_STARTUP_GAIN (2.885): exponential growth, ~2x per round
 *   DRAIN     → BBR_DRAIN_GAIN_PACING (0.347): drain STARTUP queue
 *   PROBE_BW  → cycles 1.25, 0.75, then 1.0 for rest
 *   PROBE_RTT → 1.0 (neutral)
 *
 * Uses max_bw (windowed max over BBR_MAX_BW_FILTER_LEN rounds) so that
 * transient dips from sender-idle or ACK aggregation do not collapse the
 * pacing rate.  Falls back to the EWMA-smoothed bw if the filter is not
 * yet populated.
 */
void bbr_update_pacing_rate(struct bbr_data *b)
{
    if (!b) return;

    uint32_t gain = BBR_UNIT;  /* default gain = 1.0 */
    uint32_t bw_for_pacing;

    /* Select the pacing gain based on current state */
    switch (b->state) {
    case BBR_STARTUP:
        gain = BBR_STARTUP_GAIN;           /* 2.885 : double each round */
        break;
    case BBR_DRAIN:
        gain = BBR_DRAIN_GAIN_PACING;      /* 0.347 : drain queue */
        break;
    case BBR_PROBE_BW:
        /* Use the cyclic gain table for proper
         * ProbeUp / ProbeDown / Cruise phases */
        gain = bbr_probe_bw_gains[b->probe_bw_phase % BBR_PROBE_BW_CYCLE_LEN];
        break;
    case BBR_PROBE_RTT:
        gain = BBR_PROBE_RTT_GAIN;         /* 1.0 : neutral */
        break;
    default:
        break;
    }

    /*
     * Use the windowed max BW for pacing if available, otherwise fall
     * back to the EWMA-smoothed bw.  The max filter is more robust:
     * it remembers the highest bottleneck BW seen in recent rounds.
     */
    if (b->max_bw > 0)
        bw_for_pacing = b->max_bw;
    else if (b->bw > 0)
        bw_for_pacing = b->bw;
    else
        bw_for_pacing = 100;  /* fallback initial rate */

    /* pacing_rate = bw_for_pacing * gain / BBR_UNIT */
    uint32_t rate = (uint32_t)(((uint64_t)bw_for_pacing * gain) >> BBR_GAIN_SCALE);
    if (rate < 10) rate = 10;  /* minimum pacing: 10 bytes/tick */
    b->pacing_rate = rate;
}

/* ── Main ACK processing ─────────────────────────────────────────────── */

void bbr_on_ack(struct bbr_data *b, uint32_t acked_bytes,
                uint32_t cwnd_segments, uint32_t rtt_ticks, uint64_t now)
{
    if (!b) return;

    /* Update min RTT — global absolute minimum */
    if (rtt_ticks > 0 && rtt_ticks < b->min_rtt) {
        b->min_rtt = rtt_ticks;
        b->min_rtt_stamp = now;
    }

    /* Track per-round minimum RTT for RTprop windowed filter */
    if (rtt_ticks > 0 && rtt_ticks < b->rtprop_round_min)
        b->rtprop_round_min = rtt_ticks;

    /* Sample delivery rate */
    bbr_sample_bw(b, acked_bytes, now);

    /* Update round tracking — detect round boundaries */
    {
        uint16_t prev_round_count = b->round_count;
        bbr_update_round(b, cwnd_segments, now);
        /* If round_count changed, a round just completed */
        if (b->round_count != prev_round_count) {
            bbr_update_rtprop(b);
            b->rtprop_round_min = UINT32_MAX;  /* reset for new round */
        }
    }

    /* ── STARTUP state ──────────────────────────────────────────────── */
    if (b->state == BBR_STARTUP) {
        /* Check if BW has plateaued at the end of this round */
        if (b->bw_hi == 0)
            b->bw_hi = b->bw;

        if (b->bw <= b->bw_hi) {
            b->startup_rounds++;
        } else {
            b->bw_hi = b->bw;
            b->startup_rounds = 0;
        }

        if (b->startup_rounds >= BBR_STARTUP_ROUNDS) {
            /* Transition to DRAIN */
            b->state = BBR_DRAIN;
#ifdef BBR_DEBUG
            kprintf("[BBR] STARTUP→DRAIN (BW=%u bytes/tick)\n", b->bw);
#endif
        }
    }

    /* ── DRAIN state ────────────────────────────────────────────────── */
    if (b->state == BBR_DRAIN) {
        /* Transition to PROBE_BW once inflight is <= bandwidth-delay product */
        uint32_t rtt_est = (b->rtprop_min_rtt > 0) ? b->rtprop_min_rtt : b->min_rtt;
        if (rtt_est > 0 && rtt_est != UINT32_MAX && b->bw > 0) {
            uint32_t bdp_segments = (b->bw * rtt_est) / 1400;
            if (bdp_segments < BBR_MIN_CWND) bdp_segments = BBR_MIN_CWND;
            /* DRAIN until cwnd <= BDP */
            b->target_cwnd = bdp_segments;
            if (cwnd_segments <= bdp_segments + 4) {
                b->state = BBR_PROBE_BW;
                b->probe_bw_phase = 0;
                b->probe_bw_last_round = b->round_count;
#ifdef BBR_DEBUG
                kprintf("[BBR] DRAIN→PROBE_BW (bdp=%u segs)\n", bdp_segments);
#endif
            }
        }
    }

    /* ── PROBE_BW state ─────────────────────────────────────────────── */
    if (b->state == BBR_PROBE_BW) {
        /*
         * BBR v1 bandwidth probing cycle.
         *
         * Phase advancement happens on round boundaries.  Each of the
         * BBR_PROBE_BW_CYCLE_LEN (8) phases lasts exactly one round.
         * Round completion is detected by bbr_update_round() above,
         * which increments b->round_count when delivered_this_round
         * exceeds the round_target.
         *
         * Phase 0 (ProbeUp):   gain=1.25 — send 25% faster to probe
         *                       whether more bandwidth is available
         * Phase 1 (ProbeDown): gain=0.75 — drain any queue formed by
         *                       the probe burst
         * Phases 2-7 (Cruise): gain=1.0  — neutral pacing until next
         *                       probe opportunity
         *
         * BBR v1 cycles through these 8 phases continuously in steady
         * state, and enters PROBE_RTT every BBR_PROBE_RTT_INTERVAL
         * seconds to refresh the min_rtt estimate.
         */
        if (b->round_count != b->probe_bw_last_round) {
            b->probe_bw_last_round = b->round_count;
            b->probe_bw_phase = (uint8_t)((b->probe_bw_phase + 1) % BBR_PROBE_BW_CYCLE_LEN);

#ifdef BBR_DEBUG
            kprintf("[BBR] PROBE_BW phase %d (gain=%u/256)\n",
                    b->probe_bw_phase,
                    bbr_probe_bw_gains[b->probe_bw_phase]);
#endif
        }

        /* Check if it's time for PROBE_RTT */
        if (b->min_rtt_stamp > 0 &&
            (now - b->min_rtt_stamp) > (uint64_t)BBR_PROBE_RTT_INTERVAL * 100) {
            b->state = BBR_PROBE_RTT;
            b->probe_rtt_done_stamp = now + BBR_PROBE_RTT_DURATION_TICKS;
            b->probe_rtt_round_done = 0;
            b->target_cwnd = BBR_MIN_CWND;
#ifdef BBR_DEBUG
            kprintf("[BBR] PROBE_BW→PROBE_RTT (min_rtt=%u ticks, now=%llu stamp=%llu)\n",
                    b->min_rtt, (unsigned long long)now,
                    (unsigned long long)b->min_rtt_stamp);
#endif
        }
    }

    /* ── PROBE_RTT state ────────────────────────────────────────────── */
    if (b->state == BBR_PROBE_RTT) {
        if (now >= b->probe_rtt_done_stamp || b->probe_rtt_round_done) {
            /* PROBE_RTT complete — return to PROBE_BW */
            b->state = BBR_PROBE_BW;
            b->probe_bw_phase = 0;
            b->probe_bw_last_round = b->round_count;
            b->min_rtt_stamp = now;  /* reset the timer */
#ifdef BBR_DEBUG
            kprintf("[BBR] PROBE_RTT done, returning to PROBE_BW (min_rtt=%u)\n",
                    b->min_rtt);
#endif
        }
    }

    /* ── Compute pacing rate ─────────────────────────────────────────── */
    bbr_update_pacing_rate(b);

    /* ── Compute target cwnd with per-state BBR v1 gains ──────────────── */
    if (b->state != BBR_PROBE_RTT) {
        uint32_t rtt_est = (b->rtprop_min_rtt > 0) ? b->rtprop_min_rtt : b->min_rtt;
        if (rtt_est > 0 && rtt_est != UINT32_MAX && b->bw > 0) {
            /* BDP in bytes = bw * rtprop */
            uint64_t bdp_bytes = (uint64_t)b->bw * rtt_est;
            uint32_t bdp_segments = (uint32_t)(bdp_bytes / 1400);

            /*
             * BBR v1 per-state cwnd gains (BBR §4.5):
             *
             * STARTUP:  cwnd_gain = pacing_gain (2.885).
             *   Allows inflight to grow exponentially alongside the pacing
             *   rate so the sender can double its delivery rate each RTT.
             *   Without this gain, the cwnd (at 1xBDP) caps inflight and
             *   prevents the pacing gain from filling the pipe.
             *
             * DRAIN:    cwnd_gain = 2.0.
             *   Keeps cwnd at 2x BDP during drain so the queue built by
             *   STARTUP can actually be drained by the low pacing gain
             *   (0.347).  A 1x BDP cwnd would cause under-utilization.
             *
             * PROBE_BW: cwnd_gain = 2.0.
             *   Allows 2x BDP inflight so the ProbeUp phase (gain=1.25)
             *   can detect available bandwidth without being capped by
             *   the cwnd.  The extra inflight is temporary — the
             *   ProbeDown phase (gain=0.75) drains any queue formed.
             *
             * PROBE_RTT: cwnd = BBR_MIN_CWND (4 segments), handled
             *   by the `if (b->state != BBR_PROBE_RTT)` guard above.
             */
            uint32_t cwnd_gain;
            switch (b->state) {
            case BBR_STARTUP:
                /* Match pacing gain for exponential inflight growth */
                cwnd_gain = BBR_STARTUP_GAIN;       /* 2.885 */
                break;
            case BBR_DRAIN:
                /* Keep pipe full while drain pacing empties queues */
                cwnd_gain = 2 * BBR_UNIT;           /* 2.0 */
                break;
            case BBR_PROBE_BW:
                /* Allow probing bursts without cwnd cap */
                cwnd_gain = 2 * BBR_UNIT;           /* 2.0 */
                break;
            default:
                cwnd_gain = BBR_UNIT;               /* 1.0 — fallback */
                break;
            }

            /* Apply cwnd gain: target = BDP_segments × cwnd_gain / 256 */
            uint32_t target = (uint32_t)(((uint64_t)bdp_segments * cwnd_gain)
                                         >> BBR_GAIN_SCALE);
            if (target < BBR_MIN_CWND) target = BBR_MIN_CWND;
            if (target > BBR_MAX_CWND) target = BBR_MAX_CWND;
            b->target_cwnd = target;
        } else {
            /* Not enough data yet — use default */
            b->target_cwnd = 32;
        }
    }
}

/* ── Packet loss / congestion event ──────────────────────────────────── */

void bbr_on_loss(struct bbr_data *b)
{
    if (!b) return;

    /* On packet loss, BBR does NOT reduce cwnd the way CUBIC does.
     * Instead, it relies on its model-based pacing to avoid building
     * queues that cause loss.  If loss does occur, we temporarily
     * cap target_cwnd to current inflight to drain any queue. */
    b->packet_conservation = 1;

    /* If we're still in STARTUP, loss means we overshot — go to DRAIN */
    if (b->state == BBR_STARTUP) {
        b->state = BBR_DRAIN;
#ifdef BBR_DEBUG
        kprintf("[BBR] Loss in STARTUP → DRAIN\n");
#endif
    }
}

/* ── Public accessors ────────────────────────────────────────────────── */

/*
 * Return the target congestion window in segments.
 * Called when the TCP stack needs to decide how much to send.
 */
uint32_t bbr_get_cwnd(struct bbr_data *b, uint32_t current_cwnd)
{
    if (!b) return current_cwnd;

    uint32_t target = b->target_cwnd;
    if (target < BBR_MIN_CWND) target = BBR_MIN_CWND;

    /* Packet conservation: during loss recovery, don't increase cwnd */
    if (b->packet_conservation) {
        if (target > current_cwnd)
            target = current_cwnd;
        b->packet_conservation = 0;  /* one-shot */
    }

    return target;
}

/*
 * Return the pacing rate in bytes per tick.
 * Used by send_tcp to space out packet sends.
 */
uint32_t bbr_get_pacing_rate(struct bbr_data *b)
{
    if (!b) return 0;
    return b->pacing_rate;
}

/*
 * Return the current BBR state as a human-readable string.
 */
const char *bbr_state_str(struct bbr_data *b)
{
    if (!b) return "?";
    switch (b->state) {
    case BBR_STARTUP:   return "STARTUP";
    case BBR_DRAIN:     return "DRAIN";
    case BBR_PROBE_BW:  return "PROBE_BW";
    case BBR_PROBE_RTT: return "PROBE_RTT";
    default:            return "UNKNOWN";
    }
}

/* ── Diagnostic: print BBR state ─────────────────────────────────────── */

void bbr_dump(struct bbr_data *b)
{
    if (!b) return;
    kprintf("[BBR] state=%s bw=%u bw_hi=%u max_bw=%u min_rtt=%u "
            "rtprop=%u pacer=%u tgt_cwnd=%u rounds=%u phase=%u\n",
            bbr_state_str(b), b->bw, b->bw_hi, b->max_bw, b->min_rtt,
            b->rtprop_min_rtt,
            b->pacing_rate, b->target_cwnd, b->round_count, b->probe_bw_phase);
}

/*
 * Check whether the connection is using BBR congestion control.
 * The caller can use this to decide which CC path to follow.
 * Returns 1 if BBR data is initialized (RTprop filter populated).
 */
int bbr_is_active(struct bbr_data *b)
{
    return b && b->rtprop_initialized && b->bw_initialized;
}

/* ── Implement: tcp_bbr_cong_avoid ────────────────── */
int tcp_bbr_cong_avoid(void *sk) { (void)sk; return 0; }
/* ── Implement: tcp_bbr_ssthresh ────────────────── */
uint32_t tcp_bbr_ssthresh(void *sk) { (void)sk; return 2; }
/* ── Implement: tcp_bbr_init ────────────────── */
int tcp_bbr_init(void *sk) { (void)sk; return 0; }
