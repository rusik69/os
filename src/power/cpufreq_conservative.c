/*
 * cpufreq_conservative.c — Conservative CPU frequency scaling governor
 *
 * The conservative governor is similar to ondemand but more gradual:
 *   - Frequency increases in 5% steps rather than jumping to max.
 *   - Frequency decreases quickly when load drops below threshold.
 *   - down_threshold = 20% (default).
 *
 * Design:
 *   - A timer fires every `sampling_rate` ticks (default 10 = 100 ms).
 *   - CPU utilization from APERF/MPERF ratio.
 *   - If load > up_threshold (80%), increase freq by 5% of available range.
 *   - If load < down_threshold (20%), decrease freq by one step immediately.
 *   - Rate limited to prevent oscillation.
 *
 * Item 112 — CPU frequency: conservative governor
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "cpufreq_ondemand.h"
#include "cpupstate.h"
#include "cpu.h"
#include "printf.h"
#include "timer.h"
#include "timers.h"
#include "smp.h"
#include "string.h"

/* ── MSR definitions ──────────────────────────────────────────────── */
#define MSR_IA32_MPERF         0x000000E7
#define MSR_IA32_APERF         0x000000E8

/* ── Conservative governor defaults ────────────────────────────────── */
#define CONSERVATIVE_SAMPLING_RATE_DEFAULT  10   /* ticks */
#define CONSERVATIVE_UP_THRESHOLD_DEFAULT   80   /* percent */
#define CONSERVATIVE_DOWN_THRESHOLD_DEFAULT 20   /* percent */
#define CONSERVATIVE_FREQ_STEP              5    /* percent of range per step */
#define CONSERVATIVE_DOWN_HYSTERESIS        2    /* consecutive low-load samples */

/* ── Per-CPU governor state ───────────────────────────────────────── */

struct cons_cpu_state {
    uint64_t last_up_tick;
    uint64_t last_down_tick;
    uint64_t saved_mperf;
    uint64_t saved_aperf;
    int low_load_count;
    int current_load;
};

#define CONS_MAX_CPUS 64
static struct cons_cpu_state cons_state[CONS_MAX_CPUS];

static int  g_sampling_rate  = CONSERVATIVE_SAMPLING_RATE_DEFAULT;
static int  g_up_threshold   = CONSERVATIVE_UP_THRESHOLD_DEFAULT;
static int  g_down_threshold = CONSERVATIVE_DOWN_THRESHOLD_DEFAULT;
static int  g_freq_step      = CONSERVATIVE_FREQ_STEP;
static int  g_running        = 0;
static int  g_timer_id       = -1;

/* ── Per-CPU accessor ─────────────────────────────────────────────── */

static inline struct cons_cpu_state *this_cons_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= CONS_MAX_CPUS) cpu = 0;
    return &cons_state[cpu];
}

/* ── Utility: compute load from APERF/MPERF ───────────────────────── */

static int compute_load(struct cons_cpu_state *state)
{
    uint64_t mperf = read_msr(MSR_IA32_MPERF);
    uint64_t aperf = read_msr(MSR_IA32_APERF);

    uint64_t dmperf = mperf - state->saved_mperf;
    uint64_t daperf = aperf - state->saved_aperf;

    state->saved_mperf = mperf;
    state->saved_aperf = aperf;

    if (dmperf == 0)
        return 0;

    uint64_t load = (daperf * 100ULL) / dmperf;
    if (load > 100) load = 100;
    return (int)load;
}

/* ── Governor decision logic ──────────────────────────────────────── */

static void conservative_evaluate_cpu(struct cons_cpu_state *state)
{
    int num_states = cpupstate_get_count();
    if (num_states <= 0)
        return;

    int current_state = cpupstate_get_state();
    if (current_state < 0 || current_state >= num_states)
        current_state = 0;

    int load = compute_load(state);
    state->current_load = load;

    uint64_t now = timer_get_ticks();

    /* Scale-up: gradual 5% step increase */
    if (load >= g_up_threshold) {
        if (current_state > 0 &&
            (int64_t)(now - state->last_up_tick) >= g_sampling_rate) {
            /* Calculate step in terms of P-state levels */
            int step = (num_states * g_freq_step) / 100;
            if (step < 1) step = 1;

            int new_state = current_state - step;
            if (new_state < 0) new_state = 0;

            if (new_state != current_state) {
                cpupstate_set_state(new_state);
                state->last_up_tick = now;
                kprintf("[conservative] scale UP: load=%d%% -> P%d\n",
                        load, new_state);
            }
        }
        state->low_load_count = 0;
        return;
    }

    /* Scale-down: fast decrease when below down_threshold */
    if (load <= g_down_threshold) {
        state->low_load_count++;
    } else {
        state->low_load_count = 0;
        return;
    }

    if (state->low_load_count >= CONSERVATIVE_DOWN_HYSTERESIS &&
        current_state < num_states - 1 &&
        (int64_t)(now - state->last_down_tick) >= g_sampling_rate) {
        /* Decrease by one step when below threshold */
        int step = (num_states * g_freq_step) / 100;
        if (step < 1) step = 1;

        int new_state = current_state + step;
        if (new_state >= num_states) new_state = num_states - 1;

        if (new_state != current_state) {
            cpupstate_set_state(new_state);
            state->last_down_tick = now;
            kprintf("[conservative] scale DOWN: load=%d%% -> P%d\n",
                    load, new_state);
        }
        state->low_load_count = 0;
    }
}

/* ── Timer callback ───────────────────────────────────────────────── */

static void conservative_timer_cb(void *arg)
{
    (void)arg;
    if (!g_running) return;

    struct cons_cpu_state *state = this_cons_state();
    conservative_evaluate_cpu(state);

    g_timer_id = timer_schedule(conservative_timer_cb, NULL, (uint64_t)g_sampling_rate);
}

/* ── Public API ───────────────────────────────────────────────────── */

int cpufreq_conservative_init(void)
{
    if (!cpupstate_is_present()) {
        kprintf("[conservative] CPU freq scaling not present — disabled\n");
        return -1;
    }

    for (int i = 0; i < CONS_MAX_CPUS; i++) {
        cons_state[i].last_up_tick    = 0;
        cons_state[i].last_down_tick  = 0;
        cons_state[i].saved_mperf     = read_msr(MSR_IA32_MPERF);
        cons_state[i].saved_aperf     = read_msr(MSR_IA32_APERF);
        cons_state[i].low_load_count  = 0;
        cons_state[i].current_load    = 0;
    }

    kprintf("[conservative] Governor initialized (rate=%d ticks, up=%d%%, down=%d%%, step=%d%%)\n",
            g_sampling_rate, g_up_threshold, g_down_threshold, g_freq_step);
    return 0;
}

int cpufreq_conservative_start(void)
{
    if (g_running) return 0;
    if (!cpupstate_is_present()) return -1;

    g_running = 1;
    g_timer_id = timer_schedule(conservative_timer_cb, NULL, 1);

    kprintf("[conservative] Sampling started (every %d ticks)\n", g_sampling_rate);
    return 0;
}

void cpufreq_conservative_stop(void)
{
    if (!g_running) return;
    g_running = 0;

    if (g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = -1;
    }

    kprintf("[conservative] Sampling stopped\n");
}

int cpufreq_conservative_is_active(void)
{
    return g_running;
}

void cpufreq_conservative_evaluate(void)
{
    if (!g_running || !cpupstate_is_present()) return;
    struct cons_cpu_state *state = this_cons_state();
    if (state)
        conservative_evaluate_cpu(state);
}

/* Tunable accessors */
int conservative_get_sampling_rate(void) { return g_sampling_rate; }
int conservative_set_sampling_rate(int ticks) {
    if (ticks < 1 || ticks > 1000) return -1;
    g_sampling_rate = ticks;
    if (g_running && g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = timer_schedule(conservative_timer_cb, NULL, (uint64_t)g_sampling_rate);
    }
    return 0;
}
int conservative_get_up_threshold(void) { return g_up_threshold; }
int conservative_set_up_threshold(int pct) {
    if (pct < 1 || pct > 100) return -1;
    g_up_threshold = pct;
    return 0;
}
int conservative_get_down_threshold(void) { return g_down_threshold; }
int conservative_set_down_threshold(int pct) {
    if (pct < 1 || pct > 100) return -1;
    g_down_threshold = pct;
    return 0;
}
int conservative_get_freq_step(void) { return g_freq_step; }
int conservative_set_freq_step(int pct) {
    if (pct < 1 || pct > 100) return -1;
    g_freq_step = pct;
    return 0;
}

/* ── Stub: cs_speed_up ─────────────────────────────── */
int cs_speed_up(int cpu)
{
    (void)cpu;
    kprintf("[cpufreq] cs_speed_up: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cs_slow_down ─────────────────────────────── */
int cs_slow_down(int cpu)
{
    (void)cpu;
    kprintf("[cpufreq] cs_slow_down: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: cs_target ─────────────────────────────── */
int cs_target(int cpu, unsigned int target_freq)
{
    (void)cpu;
    (void)target_freq;
    kprintf("[cpufreq] cs_target: not yet implemented\n");
    return -ENOSYS;
}
