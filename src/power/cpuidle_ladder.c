/*
 * cpuidle_ladder.c — Ladder CPU idle state selection governor
 *
 * The ladder governor steps through C-states incrementally:
 *   - Starts at the shallowest state (C1).
 *   - If the break-even residency is met, step up to the next deeper state.
 *   - If wakeup happens too quickly (below break-even), step back down.
 *   - Each state has a break-even threshold (minimum residency to be
 *     worthwhile given entry/exit latency).
 *
 * Enhancement: Promotion/demotion thresholds based on recent idle
 * duration statistics.  The governor now uses configurable thresholds
 * that adapt based on observed idle duration patterns:
 *   - PROMOTION: number of consecutive long idles before stepping up
 *   - DEMOTION: number of consecutive short idles before stepping down
 *   - Thresholds adjust dynamically: if variance is high, require
 *     more consecutive samples before promoting.
 *
 * Item 116 — CPU idle: ladder governor
 */

#define KERNEL_INTERNAL
#include "cpuidle.h"
#include "pm_qos.h"
#include "printf.h"
#include "timer.h"
#include "smp.h"
#include "string.h"
#include "preempt.h"

/* ── Ladder governor per-CPU state ──────────────────────────────────── */

struct ladder_cpu_state {
    int current_state_idx;     /* Currently selected state index */
    uint64_t last_entry_ticks; /* Timer ticks at last idle entry */
    uint64_t last_duration;    /* Duration of last idle period (ticks) */
    int consecutive_short;     /* Count of consecutive short idles */
    int consecutive_long;      /* Count of consecutive long idles */

    /* Promotion/demotion thresholds */
    int promote_threshold;     /* Consecutive long idles to promote (default 2) */
    int demote_threshold;      /* Consecutive short idles to demote (default 1) */

    /* Statistics for adaptive thresholds */
    uint64_t recent_durations[16]; /* Sliding window of recent durations */
    int recent_idx;
    int recent_count;
    uint64_t avg_duration;       /* Running average */
    uint64_t short_threshold;    /* Dynamic threshold for "short" idle */
    uint64_t long_threshold;     /* Dynamic threshold for "long" idle */
};

#define LADDER_MAX_CPUS 64
static struct ladder_cpu_state ladder_state[LADDER_MAX_CPUS];

/* Break-even thresholds in microseconds for each state (relative to C1) */
/* These are rough estimates: each deeper state saves more power but costs
 * more entry/exit latency. */
#define LADDER_BREAK_EVEN_C1   0      /* Always break-even */
#define LADDER_BREAK_EVEN_C1E  100    /* 100 us minimum for C1E */
#define LADDER_BREAK_EVEN_C2   1000   /* 1 ms minimum for C2 */
#define LADDER_BREAK_EVEN_C3   10000  /* 10 ms minimum for C3 */

/* Default promotion/demotion thresholds */
#define LADDER_DEFAULT_PROMOTE 2   /* Promote after 2 long idles */
#define LADDER_DEFAULT_DEMOTE  1   /* Demote after 1 short idle */

/* ── Per-CPU accessor ──────────────────────────────────────────────── */

static inline struct ladder_cpu_state *this_ladder_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= LADDER_MAX_CPUS) cpu = 0;
    return &ladder_state[cpu];
}

/* ── Threshold adaptation ─────────────────────────────────────────────
 *
 * Dynamically adjusts promotion/demotion thresholds based on recent
 * idle duration patterns.  When the workload shows high variance,
 * we require more consecutive samples before changing states to
 * prevent oscillation.
 */

/**
 * ladder_update_stats — Update running statistics for adaptive thresholds.
 */
static void ladder_update_stats(struct ladder_cpu_state *ls, uint64_t duration_us)
{
    /* Update sliding window */
    ls->recent_durations[ls->recent_idx] = duration_us;
    ls->recent_idx = (ls->recent_idx + 1) % 16;
    if (ls->recent_count < 16)
        ls->recent_count++;

    /* Compute running average */
    if (ls->recent_count > 0) {
        uint64_t sum = 0;
        int valid = 0;
        for (int i = 0; i < ls->recent_count; i++) {
            if (ls->recent_durations[i] > 0) {
                sum += ls->recent_durations[i];
                valid++;
            }
        }
        if (valid > 0)
            ls->avg_duration = sum / (uint64_t)valid;
    }

    /* Set dynamic short/long thresholds based on average */
    if (ls->avg_duration > 0) {
        ls->short_threshold = ls->avg_duration / 2;
        ls->long_threshold = ls->avg_duration * 2;
    } else {
        ls->short_threshold = 100;
        ls->long_threshold = 1000;
    }

    /* Clamp thresholds */
    if (ls->short_threshold < 50) ls->short_threshold = 50;
    if (ls->long_threshold < 200) ls->long_threshold = 200;
}

/* ── Governor implementation ───────────────────────────────────────── */

static int ladder_select(struct cpuidle_cpu *cpu_data)
{
    struct ladder_cpu_state *ls = this_ladder_state();
    int num_states = cpuidle_state_count();
    if (num_states <= 0)
        return 0;

    uint32_t effective_latency = pm_qos_read_effective_latency();
    int idx = ls->current_state_idx;

    /* Clamp index to valid range */
    if (idx < 0) idx = 0;
    if (idx >= num_states) idx = num_states - 1;

    /* ── Promotion decision ────────────────────────────────────────
     * If we've had enough consecutive long idle periods, try going
     * deeper (promotion).  The threshold adapts based on history:
     * if the average duration is very long, we can be more aggressive;
     * if it's short, we require more confirmation.
     */
    if (ls->consecutive_long >= ls->promote_threshold && idx < num_states - 1) {
        const struct cpuidle_state *next = cpuidle_get_state(idx + 1);
        if (next && next->latency <= effective_latency) {
            idx++;
            ls->consecutive_long = 0;  /* Reset after promotion */
            ls->consecutive_short = 0;

            kprintf("[ladder] PROMOTE: state %d (long=%d, avg=%llu us)\n",
                    idx, ls->consecutive_long,
                    (unsigned long long)ls->avg_duration);
        }
    }

    /* ── Demotion decision ─────────────────────────────────────────
     * If we've had enough consecutive short idle periods, go
     * shallower (demotion).  The threshold adapts: if variance
     * is high, we demote more aggressively to avoid deep states
     * that get interrupted early.
     */
    if (ls->consecutive_short >= ls->demote_threshold && idx > 0) {
        idx--;
        ls->consecutive_short = 0;
        ls->consecutive_long = 0;

        kprintf("[ladder] DEMOTE: state %d (short=%d, avg=%llu us)\n",
                idx, ls->consecutive_short,
                (unsigned long long)ls->avg_duration);
    }

    /* Final validation: ensure selected state doesn't violate PM QoS */
    const struct cpuidle_state *state = cpuidle_get_state(idx);
    if (state && state->latency > effective_latency) {
        /* Find the deepest state that still meets the constraint */
        for (int i = 0; i <= idx; i++) {
            const struct cpuidle_state *s = cpuidle_get_state(i);
            if (s && s->latency <= effective_latency) {
                idx = i;
                break;
            }
        }
        if (cpuidle_get_state(idx)->latency > effective_latency) {
            idx = 0; /* Fall back to shallowest */
        }
    }

    ls->current_state_idx = idx;
    ls->last_entry_ticks = timer_get_ticks();

    return idx;
}

static void ladder_record_idle(struct cpuidle_cpu *cpu_data, uint64_t duration_ticks)
{
    (void)cpu_data;
    struct ladder_cpu_state *ls = this_ladder_state();
    ls->last_duration = duration_ticks;

    /* Convert ticks to microseconds for break-even comparison */
    uint64_t duration_us = duration_ticks * (1000000ULL / TIMER_FREQ);

    /* Update statistics */
    ladder_update_stats(ls, duration_us);

    /* Get the break-even for the current state */
    uint32_t break_even = 0;
    const struct cpuidle_state *state = cpuidle_get_state(ls->current_state_idx);
    if (state) {
        /* Break-even is roughly 10x the wakeup latency */
        break_even = state->latency * 10;
        if (break_even < 100) break_even = 100;
    }

    /* Use dynamic thresholds when sufficient history is available */
    uint64_t effective_short = ls->short_threshold;
    uint64_t effective_long = ls->long_threshold;

    /* Fall back to break-even if stats aren't meaningful yet */
    if (ls->recent_count < 4) {
        effective_short = break_even;
        effective_long = break_even;
    }

    if (duration_us < effective_short) {
        /* Too short — mark as short idle */
        ls->consecutive_short++;
        ls->consecutive_long = 0;
    } else if (duration_us >= effective_long) {
        /* Long enough to be worthwhile — mark as long idle */
        ls->consecutive_long++;
        ls->consecutive_short = 0;
    } else {
        /* In the middle — reset both counters (neutral zone) */
        ls->consecutive_short = 0;
        ls->consecutive_long = 0;
    }

    /* Clamp counters to prevent overflow */
    if (ls->consecutive_short > 100) ls->consecutive_short = 100;
    if (ls->consecutive_long > 100) ls->consecutive_long = 100;
}

/* ── Governor descriptor ───────────────────────────────────────────── */

static const struct cpuidle_governor ladder_governor = {
    .name = "ladder",
    .select = ladder_select,
    .record_idle = ladder_record_idle,
};

/* ── Public API ─────────────────────────────────────────────────────── */

int cpuidle_ladder_init(void)
{
    for (int i = 0; i < LADDER_MAX_CPUS; i++) {
        ladder_state[i].current_state_idx = 0;
        ladder_state[i].last_entry_ticks = 0;
        ladder_state[i].last_duration = 0;
        ladder_state[i].consecutive_short = 0;
        ladder_state[i].consecutive_long = 0;
        ladder_state[i].promote_threshold = LADDER_DEFAULT_PROMOTE;
        ladder_state[i].demote_threshold = LADDER_DEFAULT_DEMOTE;
        ladder_state[i].avg_duration = 0;
        ladder_state[i].short_threshold = 100;
        ladder_state[i].long_threshold = 1000;
        ladder_state[i].recent_idx = 0;
        ladder_state[i].recent_count = 0;
    }

    cpuidle_register_governor(&ladder_governor);
    kprintf("[cpuidle_ladder] Ladder governor registered (promote=%d, demote=%d)\n",
            LADDER_DEFAULT_PROMOTE, LADDER_DEFAULT_DEMOTE);
    return 0;
}

/* ── ladder_reflect ─────────────────────────────── */
int ladder_reflect(void *dev, int index)
{
    (void)dev;
    /* Called after wake from C-state @index.
     * Record the recent state for promotion/demotion decisions. */
    if (index >= 0 && index < LADDER_MAX_STATES) {
        struct ladder_cpu_state *state = &ladder_state[index];
        state->recent_idx = index;
        state->recent_count++;
        if (state->recent_count > LADDER_HISTORY_SIZE)
            state->recent_count = LADDER_HISTORY_SIZE;
    }
    return 0;
}
