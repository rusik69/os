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
 * This is a simple, deterministic governor ideal for predictable workloads.
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

/* ── Per-CPU accessor ──────────────────────────────────────────────── */

static inline struct ladder_cpu_state *this_ladder_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= LADDER_MAX_CPUS) cpu = 0;
    return &ladder_state[cpu];
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

    /* If we've been having long idle periods, try going deeper */
    if (ls->consecutive_long >= 2 && idx < num_states - 1) {
        /* Check the next state doesn't exceed latency constraint */
        const struct cpuidle_state *next = cpuidle_get_state(idx + 1);
        if (next && next->latency <= effective_latency) {
            idx++;
        }
    }

    /* If we've been having short idle periods, go shallower */
    if (ls->consecutive_short >= 1 && idx > 0) {
        idx--;
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

    /* Get the break-even for the current state */
    uint32_t break_even = 0;
    const struct cpuidle_state *state = cpuidle_get_state(ls->current_state_idx);
    if (state) {
        /* Break-even is roughly 10x the wakeup latency */
        break_even = state->latency * 10;
        if (break_even < 100) break_even = 100;
    }

    if (duration_us < break_even) {
        /* Too short — mark as short idle */
        ls->consecutive_short++;
        ls->consecutive_long = 0;
    } else {
        /* Long enough to be worthwhile — mark as long idle */
        ls->consecutive_long++;
        ls->consecutive_short = 0;
    }

    /* Clamp counters to prevent overflow */
    if (ls->consecutive_short > 10) ls->consecutive_short = 10;
    if (ls->consecutive_long > 10) ls->consecutive_long = 10;
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
    }

    cpuidle_register_governor(&ladder_governor);
    kprintf("[cpuidle_ladder] Ladder governor registered\n");
    return 0;
}
