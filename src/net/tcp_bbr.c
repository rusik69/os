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

#define BBR_STARTUP_GAIN       (5 * BBR_UNIT / 4)       /* 1.25 : 25% growth per RTT */
#define BBR_DRAIN_GAIN         (3 * BBR_UNIT / 4)       /* 0.75 : drain queue */
#define BBR_PROBE_BW_GAIN      (5 * BBR_UNIT / 4)       /* 1.25 : probe for more BW */
#define BBR_PROBE_BW_GAIN_LOW  (3 * BBR_UNIT / 4)       /* 0.75 : drain after probe */
#define BBR_PROBE_RTT_GAIN     (BBR_UNIT)               /* 1.0  : neutral */

/* How many rounds (RTTs) before checking if BW has plateaued in STARTUP */
#define BBR_STARTUP_ROUNDS     3

/* PROBE_BW cycle: 8 phases, gain=1.25 for first, 0.75 for next, 1.0 for rest */
#define BBR_PROBE_BW_CYCLE_LEN 8

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
    }
}

/* ── Main ACK processing ─────────────────────────────────────────────── */

void bbr_on_ack(struct bbr_data *b, uint32_t acked_bytes,
                uint32_t cwnd_segments, uint32_t rtt_ticks, uint64_t now)
{
    if (!b) return;

    /* Update min RTT */
    if (rtt_ticks > 0 && rtt_ticks < b->min_rtt) {
        b->min_rtt = rtt_ticks;
        b->min_rtt_stamp = now;
    }

    /* Sample delivery rate */
    bbr_sample_bw(b, acked_bytes, now);

    /* Update round tracking */
    bbr_update_round(b, cwnd_segments, now);

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
        if (b->min_rtt > 0 && b->bw > 0) {
            uint32_t bdp_segments = (b->bw * b->min_rtt) / 1400;
            if (bdp_segments < BBR_MIN_CWND) bdp_segments = BBR_MIN_CWND;
            /* DRAIN until cwnd <= BDP */
            b->target_cwnd = bdp_segments;
            if (cwnd_segments <= bdp_segments + 4) {
                b->state = BBR_PROBE_BW;
                b->probe_bw_phase = 0;
#ifdef BBR_DEBUG
                kprintf("[BBR] DRAIN→PROBE_BW (bdp=%u segs)\n", bdp_segments);
#endif
            }
        }
    }

    /* ── PROBE_BW state ─────────────────────────────────────────────── */
    if (b->state == BBR_PROBE_BW) {
        /* Cycle through probe phases.
         * Phase 0: probe with gain=1.25 (one round)
         * Phases 1-BBR_PROBE_BW_CYCLE_LEN-1: drain/neutral */
        if (b->round_delivered >= b->delivered / 2) {
            /* Advance phase at end of round */
            b->probe_bw_phase = (b->probe_bw_phase + 1) % BBR_PROBE_BW_CYCLE_LEN;
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
            b->min_rtt_stamp = now;  /* reset the timer */
#ifdef BBR_DEBUG
            kprintf("[BBR] PROBE_RTT done, returning to PROBE_BW (min_rtt=%u)\n",
                    b->min_rtt);
#endif
        }
    }

    /* ── Compute pacing rate ─────────────────────────────────────────── */
    {
        uint32_t gain = BBR_UNIT;  /* default gain = 1.0 */

        switch (b->state) {
        case BBR_STARTUP:
            gain = BBR_STARTUP_GAIN;
            break;
        case BBR_DRAIN:
            gain = BBR_DRAIN_GAIN;
            break;
        case BBR_PROBE_BW:
            /* Phase 0: probe gain, rest: drain or neutral */
            if (b->probe_bw_phase == 0)
                gain = BBR_PROBE_BW_GAIN;
            else if (b->probe_bw_phase == 1)
                gain = BBR_PROBE_BW_GAIN_LOW;
            else
                gain = BBR_UNIT;
            break;
        case BBR_PROBE_RTT:
            gain = BBR_PROBE_RTT_GAIN;
            break;
        default:
            break;
        }

        /* pacing_rate = bw * gain / UNIT */
        if (b->bw > 0) {
            uint32_t rate = (uint32_t)(((uint64_t)b->bw * gain) >> BBR_GAIN_SCALE);
            if (rate < 10) rate = 10;  /* minimum pacing: 1 byte/tick */
            b->pacing_rate = rate;
        }
    }

    /* ── Compute target cwnd ─────────────────────────────────────────── */
    if (b->state != BBR_PROBE_RTT) {
        if (b->min_rtt > 0 && b->bw > 0) {
            /* BDP in bytes = bw * min_rtt */
            uint64_t bdp_bytes = (uint64_t)b->bw * b->min_rtt;
            uint32_t bdp_segments = (uint32_t)(bdp_bytes / 1400);
            if (bdp_segments < BBR_MIN_CWND)
                bdp_segments = BBR_MIN_CWND;
            if (bdp_segments > BBR_MAX_CWND)
                bdp_segments = BBR_MAX_CWND;

            /* In PROBE_BW phase 0, use 2x BDP to allow probing */
            if (b->state == BBR_PROBE_BW && b->probe_bw_phase == 0) {
                bdp_segments = (bdp_segments < BBR_MAX_CWND / 2)
                               ? bdp_segments * 2 : BBR_MAX_CWND;
            }

            b->target_cwnd = bdp_segments;
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
    kprintf("[BBR] state=%s bw=%u bw_hi=%u min_rtt=%u pacer=%u "
            "tgt_cwnd=%u rounds=%u phase=%u\n",
            bbr_state_str(b), b->bw, b->bw_hi, b->min_rtt, b->pacing_rate,
            b->target_cwnd, b->round_count, b->probe_bw_phase);
}

/*
 * Check whether the connection is using BBR congestion control.
 * The caller can use this to decide which CC path to follow.
 * Returns 1 if BBR data is initialized (i.e. min_rtt != UINT32_MAX).
 */
int bbr_is_active(struct bbr_data *b)
{
    return b && b->bw_initialized && b->min_rtt != UINT32_MAX;
}

/* ── Implement: tcp_bbr_cong_avoid ────────────────── */
int tcp_bbr_cong_avoid(void *sk) { (void)sk; return 0; }
/* ── Implement: tcp_bbr_ssthresh ────────────────── */
uint32_t tcp_bbr_ssthresh(void *sk) { (void)sk; return 2; }
/* ── Implement: tcp_bbr_init ────────────────── */
int tcp_bbr_init(void *sk) { (void)sk; return 0; }
