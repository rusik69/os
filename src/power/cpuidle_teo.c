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

struct teo_cpu_state {
    /* For each state, how many timer hits we've seen before/after predicted idle */
    uint64_t timer_before[TEO_MAX_STATES_HISTORY]; /* Timers that fired before expected idle end */
    uint64_t timer_after[TEO_MAX_STATES_HISTORY];  /* Timers that fired after expected idle end */
    uint64_t total_entries;                        /* Total idle entries */
    uint64_t last_duration_us;                     /* Last idle duration in us */
    int last_state_idx;                            /* Last selected state */
};

#define TEO_MAX_CPUS 64
static struct teo_cpu_state teo_state[TEO_MAX_CPUS];

/* ── Per-CPU accessor ──────────────────────────────────────────────── */

static inline struct teo_cpu_state *this_teo_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= TEO_MAX_CPUS) cpu = 0;
    return &teo_state[cpu];
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

    /* Start from the deepest state and work backward until we find one
     * where timer_before count is low enough to justify the depth. */
    int idx = num_states - 1;

    /* If no history yet, start conservatively at the second state */
    if (ts->total_entries < TEO_HISTORY_SIZE) {
        idx = 1; /* Start at C1 */
        if (idx >= num_states) idx = num_states - 1;
    } else {
        /* Walk from deepest up: if timer_before is significant for this
         * state, choose a shallower one. */
        while (idx > 0) {
            uint64_t total = ts->timer_before[idx] + ts->timer_after[idx];
            if (total > 0) {
                /* If more than 30% of hits are timer_before (i.e., we
                 * get woken early), avoid this deep state. */
                if (ts->timer_before[idx] * 10 > total * 3) {
                    idx--;
                    continue;
                }
            }
            break;
        }
    }

    /* PM QoS constraint check */
    for (int i = 0; i <= idx; i++) {
        const struct cpuidle_state *s = cpuidle_get_state(i);
        if (s && s->latency > effective_latency) {
            /* This state violates the constraint; keep searching deeper
             * (shallower state index) */
            continue;
        }
        idx = i;
        break;
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

    /* Update timer hit statistics for the last selected state */
    int state_idx = ts->last_state_idx;
    if (state_idx >= 0 && state_idx < TEO_MAX_STATES_HISTORY) {
        /* Compare actual duration to the state's break-even latency.
         * If the actual idle was shorter than the state latency * 10
         * (rough break-even), we got woken early = timer_before.
         * Otherwise, timer_after. */
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
    }

    cpuidle_register_governor(&teo_governor);
    kprintf("[cpuidle_teo] TEO governor registered\n");
    return 0;
}
