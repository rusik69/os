// SPDX-License-Identifier: GPL-2.0-only
/*
 * tcp_bbr2.c — BBRv2 congestion control with ECN/probe-RTT
 *
 * Implements BBRv2 congestion control algorithm for TCP.
 * Features: pacing, ECN support, ProbeRTT, bandwidth probing.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "timer.h"

#define BBR2_MAX_BW_FILTER  10
#define BBR2_MIN_RTT_FILTER 10
#define BBR2_PROBE_RTT_INTERVAL 10000  /* 10 seconds in ticks */
#define BBR2_PROBE_RTT_DURATION 200   /* 200ms */

struct bbr2_state {
    /* Bandwidth tracking */
    uint64_t bw_filter[BBR2_MAX_BW_FILTER];
    int bw_filter_idx;
    uint64_t max_bw;

    /* RTT tracking */
    uint64_t min_rtt;
    uint64_t rtt_filter[BBR2_MIN_RTT_FILTER];
    int rtt_filter_idx;

    /* State machine */
    int state; /* 0=startup, 1=drain, 2=probe_bw, 3=probe_rtt */
    uint64_t probe_rtt_timestamp;
    int probe_rtt_done;

    /* Pacing rate */
    uint64_t pacing_rate;
    uint64_t cwnd;
    uint64_t prior_cwnd;
    uint64_t target_cwnd;

    /* ECN */
    uint64_t ecn_marks;
    uint64_t ecn_ce_threshold;

    /* Gains */
    int round_count;
    uint64_t round_start;
    int full_bw_seen;
    int full_bw_count;
};

static struct bbr2_state bbr2;

/* Initialize BBRv2 state for a connection */
void bbr2_init(void)
{
    memset(&bbr2, 0, sizeof(bbr2));
    bbr2.min_rtt = ~0ULL;
    bbr2.state = 0; /* STARTUP */
    bbr2.ecn_marks = 0;
    bbr2.ecn_ce_threshold = 3; /* CE mark threshold */
    bbr2.target_cwnd = 3000;   /* initial cwnd */

    kprintf("[OK] BBRv2 — Congestion control with ECN/probe-RTT\n");
}

/* Update max bandwidth filter */
void bbr2_update_bw(uint64_t bw_sample)
{
    bbr2.bw_filter[bbr2.bw_filter_idx] = bw_sample;
    bbr2.bw_filter_idx = (bbr2.bw_filter_idx + 1) % BBR2_MAX_BW_FILTER;

    /* Find max */
    bbr2.max_bw = 0;
    for (int i = 0; i < BBR2_MAX_BW_FILTER; i++) {
        if (bbr2.bw_filter[i] > bbr2.max_bw)
            bbr2.max_bw = bbr2.bw_filter[i];
    }
}

/* Update min RTT */
void bbr2_update_rtt(uint64_t rtt_sample)
{
    bbr2.rtt_filter[bbr2.rtt_filter_idx] = rtt_sample;
    bbr2.rtt_filter_idx = (bbr2.rtt_filter_idx + 1) % BBR2_MIN_RTT_FILTER;

    /* Find min */
    bbr2.min_rtt = ~0ULL;
    for (int i = 0; i < BBR2_MIN_RTT_FILTER; i++) {
        if (bbr2.rtt_filter[i] > 0 && bbr2.rtt_filter[i] < bbr2.min_rtt)
            bbr2.min_rtt = bbr2.rtt_filter[i];
    }
}

/* Process ECN CE mark */
void bbr2_process_ecn_ce(void)
{
    bbr2.ecn_marks++;
    if (bbr2.ecn_marks >= bbr2.ecn_ce_threshold) {
        /* Reduce cwnd */
        bbr2.target_cwnd = (bbr2.target_cwnd * 7) / 8;
        if (bbr2.target_cwnd < 100) bbr2.target_cwnd = 100;
        bbr2.ecn_marks = 0;
    }
}

/* BBRv2 state machine */
void bbr2_tick(uint64_t now)
{
    switch (bbr2.state) {
    case 0: /* STARTUP */
        /* Increase pacing rate by 2x per round */
        bbr2.pacing_rate = bbr2.max_bw * 2;
        bbr2.cwnd = bbr2.target_cwnd;

        /* Check if full bandwidth is reached (~3 rounds without 25% gain) */
        if (bbr2.full_bw_count >= 3) {
            bbr2.state = 1; /* DRAIN */
            bbr2.full_bw_seen = 1;
        }
        break;

    case 1: /* DRAIN */
        /* Drain queue by reducing rate */
        bbr2.pacing_rate = (bbr2.max_bw * 75) / 100; /* 75% of max */
        if (bbr2.cwnd <= bbr2.target_cwnd)
            bbr2.state = 2; /* PROBE_BW */
        break;

    case 2: /* PROBE_BW */
        /* Cycle through gains to probe bandwidth */
        {
            static const int gain_cycle[8] = {125, 100, 100, 100, 100, 100, 100, 75};
            static int cycle_idx = 0;
            int gain = gain_cycle[cycle_idx % 8];
            bbr2.pacing_rate = (bbr2.max_bw * (uint64_t)gain) / 100;
            cycle_idx++;

            /* ProbeRTT check */
            if (now - bbr2.probe_rtt_timestamp > BBR2_PROBE_RTT_INTERVAL) {
                bbr2.state = 3; /* PROBE_RTT */
                bbr2.probe_rtt_timestamp = now;
                bbr2.prior_cwnd = bbr2.cwnd;
                bbr2.cwnd = bbr2.target_cwnd / 2;
            }
        }
        break;

    case 3: /* PROBE_RTT */
        /* Drain to a lower cwnd to get a fresh RTT sample */
        bbr2.cwnd = bbr2.target_cwnd / 2;
        if (now - bbr2.probe_rtt_timestamp > BBR2_PROBE_RTT_DURATION) {
            bbr2.cwnd = bbr2.prior_cwnd;
            bbr2.state = 2; /* back to PROBE_BW */
            bbr2.probe_rtt_timestamp = now;
        }
        break;
    }
}

/* Get current pacing rate */
uint64_t bbr2_get_pacing_rate(void)
{
    return bbr2.pacing_rate;
}

/* Get current congestion window */
uint64_t bbr2_get_cwnd(void)
{
    return bbr2.cwnd;
}
#include "module.h"
module_init(bbr2_init);
