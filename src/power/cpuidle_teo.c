/*
 * cpuidle_teo.c — Timer Events Oriented cpuidle governor
 *
 * The Timer Events Oriented (TEO) governor selects idle states based
 * on observed timer event patterns.  It tracks:
 *   - How many times a timer event occurred BEFORE the predicted idle
 *     duration (meaning we slept too long and got woken by a timer).
 *   - How many times a timer event occurred AFTER the predicted idle
 *     (meaning the next event is farther than we predicted).
 *
 * Using this information, the governor dynamically adjusts which
 * C-state to enter, preferring shallower states when timer events
 * tend to arrive early, and deeper states when they arrive late.
 *
 * Enhancement: Timer history pattern recognition.
 * The governor now maintains a sliding window of recent idle durations
 * and uses statistical analysis (median and variance) to predict the
 * upcoming idle duration more accurately.  This helps select the
 * appropriate C-state based on actual workload patterns rather than
 * just binary hit/miss counters.
 *
 * Item 117 — CPU idle: TEO governor
 */

#define KERNEL_INTERNAL
#include "cpuidle.h"
#include "pm_qos.h"
#include "printf.h"
#include "timer.h"
#include "smp.h"
#include "string.h"
#include "preempt.h"

/* ── TEO governor per-CPU state ─────────────────────────────────────── */

#define TEO_MAX_STATES_HISTORY  CPUIDLE_MAX_STATES
#define TEO_HISTORY_SIZE        8   /* Number of recent samples per state */

/* Pattern recognition: sliding window of idle durations */
#define TEO_PATTERN_WINDOW_SIZE 16  /* Number of recent idle durations to track */

struct teo_cpu_state {
    /* For each state, how many timer hits we've seen before/after predicted idle */
    uint64_t timer_before[TEO_MAX_STATES_HISTORY]; /* Timers that fired before expected idle end */
    uint64_t timer_after[TEO_MAX_STATES_HISTORY];  /* Timers that fired after expected idle end */
    uint64_t total_entries;                        /* Total idle entries */
    uint64_t last_duration_us;                     /* Last idle duration in us */
    int last_state_idx;                            /* Last selected state */

    /* Timer history pattern recognition */
    uint64_t pattern_window[TEO_PATTERN_WINDOW_SIZE]; /* Sliding window of idle durations */
    int pattern_window_idx;                            /* Current index in sliding window */
    int pattern_window_count;                          /* Number of valid entries */
    uint64_t pattern_median;                           /* Median of recent durations */
    uint64_t pattern_variance;                         /* Variance of recent durations */
    int pattern_trend;                                 /* 1=increasing, -1=decreasing, 0=stable */
};

#define TEO_MAX_CPUS 64

/* ── TEO governor constants ─────────────────────────────── */
#ifndef TEO_MAX_STATES
#define TEO_MAX_STATES   10
#endif
#ifndef TEO_PATTERN_WINDOW
#define TEO_PATTERN_WINDOW 5
#endif

static struct teo_cpu_state teo_state[TEO_MAX_CPUS];

/* ── Per-CPU accessor ──────────────────────────────────────────────── */

static inline struct teo_cpu_state *this_teo_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= TEO_MAX_CPUS) cpu = 0;
    return &teo_state[cpu];
}

/* ── Pattern recognition helpers ───────────────────────────────────────
 *
 * Maintains a sliding window of recent idle durations and computes
 * statistical properties to predict the upcoming idle length.
 */

/**
 * teo_update_pattern_window — Add a new idle duration to the sliding window.
 */
static void teo_update_pattern_window(struct teo_cpu_state *ts, uint64_t duration_us)
{
    ts->pattern_window[ts->pattern_window_idx] = duration_us;
    ts->pattern_window_idx = (ts->pattern_window_idx + 1) % TEO_PATTERN_WINDOW_SIZE;
    if (ts->pattern_window_count < TEO_PATTERN_WINDOW_SIZE)
        ts->pattern_window_count++;
}

/**
 * teo_compute_pattern_stats — Compute median and variance of recent durations.
 *
 * Uses a simple selection algorithm to find the median and computes
 * variance as the mean squared deviation from the median.
 */
static void teo_compute_pattern_stats(struct teo_cpu_state *ts)
{
    if (ts->pattern_window_count == 0) {
        ts->pattern_median = 0;
        ts->pattern_variance = 0;
        ts->pattern_trend = 0;
        return;
    }

    /* Copy window to temporary array for sorting */
    uint64_t sorted[TEO_PATTERN_WINDOW_SIZE] = {0};
    int n = ts->pattern_window_count;
    for (int i = 0; i < n; i++)
        sorted[i] = ts->pattern_window[i];

    /* Simple bubble sort (n <= 16, so this is fine) */
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (sorted[j] > sorted[j + 1]) {
                uint64_t tmp = sorted[j];
                sorted[j] = sorted[j + 1];
                sorted[j + 1] = tmp;
            }
        }
    }

    /* Median */
    ts->pattern_median = sorted[n / 2];

    /* Variance (mean squared deviation from median) */
    uint64_t sum_sq = 0;
    for (int i = 0; i < n; i++) {
        int64_t diff = (int64_t)(sorted[i] - ts->pattern_median);
        uint64_t sq = (uint64_t)(diff < 0 ? -diff : diff);
        sum_sq += sq * sq;
    }
    ts->pattern_variance = sum_sq / (uint64_t)n;

    /* Trend: compare first half to second half */
    if (n >= 4) {
        uint64_t first_half_sum = 0, second_half_sum = 0;
        int half = n / 2;
        for (int i = 0; i < half; i++)
            first_half_sum += sorted[i];
        for (int i = half; i < n; i++)
            second_half_sum += sorted[i];

        uint64_t first_avg = first_half_sum / (uint64_t)half;
        uint64_t second_avg = second_half_sum / (uint64_t)(n - half);

        if (second_avg > first_avg + first_avg / 10)
            ts->pattern_trend = 1;  /* Increasing */
        else if (first_avg > second_avg + second_avg / 10)
            ts->pattern_trend = -1; /* Decreasing */
        else
            ts->pattern_trend = 0;  /* Stable */
    }
}

/**
 * teo_predict_idle_duration — Predict the next idle duration using pattern history.
 *
 * Uses the median of recent durations as a robust prediction,
 * adjusted for the observed trend.
 *
 * Returns predicted duration in microseconds.
 */
static uint64_t teo_predict_idle_duration(struct teo_cpu_state *ts)
{
    uint64_t predicted = ts->pattern_median;

    /* Adjust for trend */
    if (ts->pattern_trend > 0 && predicted > 0)
        predicted = predicted * 12 / 10;  /* +20% if increasing */
    else if (ts->pattern_trend < 0)
        predicted = predicted * 8 / 10;   /* -20% if decreasing */

    /* If variance is high, be conservative (predict shorter) */
    if (ts->pattern_variance > predicted * predicted && predicted > 100)
        predicted = predicted * 7 / 10;   /* -30% if high variance */

    if (predicted < 10) predicted = 10;  /* minimum 10 us */

    return predicted;
}

/* ── Governor implementation ───────────────────────────────────────── */

static int teo_select(struct cpuidle_cpu *cpu_data)
{
    (void)cpu_data;
    struct teo_cpu_state *ts = this_teo_state();
    int num_states = cpuidle_state_count();
    if (num_states <= 0)
        return 0;

    uint32_t effective_latency = pm_qos_read_effective_latency();

    /* Use pattern prediction to find the best state */
    uint64_t predicted_duration = teo_predict_idle_duration(ts);

    /* Start from the deepest state that fits the predicted duration */
    int idx = 0;
    for (int i = 0; i < num_states; i++) {
        const struct cpuidle_state *s = cpuidle_get_state(i);
        if (!s) continue;

        /* Skip states that violate PM QoS */
        if (s->latency > effective_latency)
            continue;

        /* For deeper states, check that predicted duration exceeds
         * the state's break-even residency (latency * 10) */
        if (predicted_duration > 0 && i > 0) {
            uint32_t break_even = s->latency * 10;
            if (break_even < 100) break_even = 100;

            if (predicted_duration < (uint64_t)break_even)
                break;  /* Prediction says we won't stay long enough */
        }

        idx = i;
    }

    /* If no history yet, use the simpler timer_before/after approach */
    if (ts->total_entries < TEO_HISTORY_SIZE) {
        idx = 1; /* Start at C1 */
        if (idx >= num_states) idx = num_states - 1;
    } else if (ts->pattern_window_count < 4) {
        /* Not enough pattern data: use original TEO logic */
        idx = num_states - 1;
        while (idx > 0) {
            uint64_t total = ts->timer_before[idx] + ts->timer_after[idx];
            if (total > 0) {
                if (ts->timer_before[idx] * 10 > total * 3) {
                    idx--;
                    continue;
                }
            }
            break;
        }
    }

    /* Ensure idx is valid */
    if (idx < 0) idx = 0;
    if (idx >= num_states) idx = num_states - 1;

    ts->last_state_idx = idx;

    return idx;
}

static void teo_record_idle(struct cpuidle_cpu *cpu_data, uint64_t duration_ticks)
{
    (void)cpu_data;
    struct teo_cpu_state *ts = this_teo_state();
    uint64_t duration_us = duration_ticks * (1000000ULL / TIMER_FREQ);
    ts->last_duration_us = duration_us;
    ts->total_entries++;

    /* Update pattern recognition window */
    teo_update_pattern_window(ts, duration_us);
    teo_compute_pattern_stats(ts);

    /* Update timer hit statistics for the last selected state */
    int state_idx = ts->last_state_idx;
    if (state_idx >= 0 && state_idx < TEO_MAX_STATES_HISTORY) {
        const struct cpuidle_state *state = cpuidle_get_state(state_idx);
        uint32_t break_even = 100; /* default 100 us */
        if (state) {
            break_even = state->latency * 10;
            if (break_even < 100) break_even = 100;
        }

        if (duration_us < break_even) {
            ts->timer_before[state_idx]++;
        } else {
            ts->timer_after[state_idx]++;
        }

        /* Prevent counter saturation — divide by 2 when thresholds reached */
        if (ts->timer_before[state_idx] > 1000000) {
            ts->timer_before[state_idx] >>= 1;
            ts->timer_after[state_idx] >>= 1;
        }
        if (ts->timer_after[state_idx] > 1000000) {
            ts->timer_before[state_idx] >>= 1;
            ts->timer_after[state_idx] >>= 1;
        }
    }
}

/* ── Governor descriptor ───────────────────────────────────────────── */

static const struct cpuidle_governor teo_governor = {
    .name = "teo",
    .select = teo_select,
    .record_idle = teo_record_idle,
};

/* ── Public API ─────────────────────────────────────────────────────── */

int cpuidle_teo_init(void)
{
    for (int i = 0; i < TEO_MAX_CPUS; i++) {
        memset(&teo_state[i], 0, sizeof(teo_state[i]));
        teo_state[i].last_state_idx = 0;
        teo_state[i].pattern_window_idx = 0;
        teo_state[i].pattern_window_count = 0;
        teo_state[i].pattern_median = 0;
        teo_state[i].pattern_variance = 0;
        teo_state[i].pattern_trend = 0;
    }

    cpuidle_register_governor(&teo_governor);
    kprintf("[cpuidle_teo] TEO governor registered (pattern recognition enabled)\n");
    return 0;
}

/* ── teo_reflect ─────────────────────────────── */
int teo_reflect(void *dev, int index)
{
    (void)dev;
    /* Called after wake from C-state @index.
     * Update the TEO pattern window for idle duration prediction. */
    if (index >= 0 && index < TEO_MAX_STATES) {
        struct teo_cpu_state *state = &teo_state[index];
        state->last_state_idx = index;
        state->pattern_window_idx = (state->pattern_window_idx + 1) % TEO_PATTERN_WINDOW;
        state->pattern_window_count++;
        if (state->pattern_window_count > TEO_PATTERN_WINDOW)
            state->pattern_window_count = TEO_PATTERN_WINDOW;
    }
    return 0;
}
