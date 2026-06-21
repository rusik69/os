/*
 * cpufreq_ondemand.c — OnDemand CPU frequency scaling governor
 *
 * Periodically samples CPU utilization (via APERF/MPERF MSR ratio) and
 * adjusts the P-state to match workload demand.
 *
 * Design:
 *   - A timer fires every `sampling_rate` ticks (default 10 = 100 ms).
 *   - CPU utilization is inferred from the APERF/MPERF ratio:
 *       util = aperf_delta / mperf_delta  (ratio of actual cycles to P0 cycles)
 *   - If util > up_threshold, we scale up one P-state at a time.
 *   - If util < down_threshold after several consecutive low samples,
 *     we scale down.
 *   - Rate limiting: At most one transition every min_sample_rate ticks.
 *
 * Tunable parameters are exposed through /sys/devices/system/cpu/cpu0/cpufreq/
 * when governor is "ondemand":
 *   - sampling_rate     (ticks between samples)
 *   - up_threshold      (load % to trigger scale-up)
 *   - down_threshold    (load % to trigger scale-down)
 *   - ignore_nice       (whether to count idle as "nice" wait)
 *
 * Reference: Linux drivers/cpufreq/cpufreq_ondemand.c
 *
 * Item 104 — CPU frequency: ondemand governor
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "cpufreq_ondemand.h"
#include "cpupstate.h"
#include "cpu.h"           /* read_msr */
#include "printf.h"
#include "timer.h"         /* TIMER_FREQ */
#include "timers.h"        /* timer_schedule, timer_cancel */
#include "smp.h"           /* smp_get_cpu_count, smp_get_cpu_id */
#include "string.h"

/* ── MSR definitions ──────────────────────────────────────────────── */
#define MSR_IA32_MPERF         0x000000E7
#define MSR_IA32_APERF         0x000000E8

/* ── Per-CPU governor state ───────────────────────────────────────── */

struct od_cpu_state {
    /* Ticks at which we last scaled up/down */
    uint64_t last_up_tick;
    uint64_t last_down_tick;

    /* Saved APERF/MPERF for utilization calculation */
    uint64_t saved_mperf;
    uint64_t saved_aperf;

    /* Consecutive low-load samples before scaling down */
    int low_load_count;

    /* Current load estimate (0-100) */
    int current_load;
};

/* Max CPUs we track (matches SMP_MAX_CPUS) */
#define OD_MAX_CPUS 64

static struct od_cpu_state od_state[OD_MAX_CPUS];

/* ── Governor tunables ────────────────────────────────────────────── */

static int  g_sampling_rate   = ONDEMAND_SAMPLING_RATE_DEFAULT;  /* ticks */
static int  g_up_threshold    = ONDEMAND_UP_THRESHOLD_DEFAULT;   /* percent */
static int  g_down_threshold  = ONDEMAND_DOWN_THRESHOLD_DEFAULT; /* percent */
static int  g_ignore_nice     = 0;
static int  g_running         = 0;   /* 1 = sampling active */
static int  g_timer_id        = -1;  /* periodic timer ID */

/* Sanity bounds for tunables */
#define ONDEMAND_MIN_THRESHOLD   1
#define ONDEMAND_MAX_THRESHOLD   100
#define ONDEMAND_MIN_RATE_TICKS  1
#define ONDEMAND_MAX_RATE_TICKS  1000

/* Number of consecutive low-load samples before scaling down (anti-flap) */
#define ONDEMAND_DOWN_HYSTERESIS  2

/* ── Forward declarations ─────────────────────────────────────────── */

static void ondemand_timer_cb(void *arg);

/* ── Per-CPU accessor ─────────────────────────────────────────────── */

static inline struct od_cpu_state *this_od_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= OD_MAX_CPUS) cpu = 0;
    return &od_state[cpu];
}

/* ── Tunable accessors ────────────────────────────────────────────── */

int ondemand_get_sampling_rate(void)
{
    return g_sampling_rate;
}

int ondemand_set_sampling_rate(int ticks)
{
    if (ticks < ONDEMAND_MIN_RATE_TICKS || ticks > ONDEMAND_MAX_RATE_TICKS)
        return -1;
    g_sampling_rate = ticks;
    /* If sampling is active, reschedule the timer */
    if (g_running && g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = timer_schedule(ondemand_timer_cb, NULL, (uint64_t)g_sampling_rate);
    }
    return 0;
}

int ondemand_get_up_threshold(void)
{
    return g_up_threshold;
}

int ondemand_set_up_threshold(int pct)
{
    if (pct < ONDEMAND_MIN_THRESHOLD || pct > ONDEMAND_MAX_THRESHOLD)
        return -1;
    g_up_threshold = pct;
    return 0;
}

int ondemand_get_down_threshold(void)
{
    return g_down_threshold;
}

int ondemand_set_down_threshold(int pct)
{
    if (pct < ONDEMAND_MIN_THRESHOLD || pct > ONDEMAND_MAX_THRESHOLD)
        return -1;
    g_down_threshold = pct;
    return 0;
}

int ondemand_get_ignore_nice(void)
{
    return g_ignore_nice;
}

void ondemand_set_ignore_nice(int val)
{
    g_ignore_nice = val ? 1 : 0;
}

/* Return 1 if ondemand governor is actively sampling */
int cpufreq_ondemand_is_active(void)
{
    return g_running;
}

/* ── Utility: Read CPU utilization from APERF/MPERF ─────────────────
 *
 * APERF (Actual Performance) counts cycles at the actual core frequency.
 * MPERF (Maximum Performance) counts cycles at P0 (max) frequency.
 * Both count only while the CPU is not halted (i.e., not in a C-state
 * deeper than C0).
 *
 * utilization = ((aperf - last_aperf) * 100) / (mperf - last_mperf)
 *
 * When the CPU is in C1+ (halt), both counters stall, so the ratio
 * naturally represents the proportion of non-idle time at the current
 * frequency.
 *
 * If the CPU is idle (mperf didn't advance), we report 0% load.
 */
static int compute_load(struct od_cpu_state *state)
{
    uint64_t mperf = read_msr(MSR_IA32_MPERF);
    uint64_t aperf = read_msr(MSR_IA32_APERF);

    uint64_t dmperf = mperf - state->saved_mperf;
    uint64_t daperf = aperf - state->saved_aperf;

    /* Save for next call */
    state->saved_mperf = mperf;
    state->saved_aperf = aperf;

    /* If MPERF didn't advance, the CPU has been idle (halted).
     * We report 0% load in this case. */
    if (dmperf == 0)
        return 0;

    /* Load % = (daperf * 100) / dmperf, clamped to 0-100 */
    uint64_t load = (daperf * 100ULL) / dmperf;
    if (load > 100) load = 100;

    return (int)load;
}

/* ── Helper: get frequency (MHz) for a P-state ───────────────────── */
static uint32_t pstate_freq_mhz(int state)
{
    struct cpupstate_state info;
    if (cpupstate_get_info(state, &info) < 0)
        return 0;
    return info.core_freq;
}

/* ── Governor decision logic ────────────────────────────────────────
 *
 * This is called from the timer callback.  It reads the current CPU
 * utilization, compares against thresholds, and adjusts the P-state
 * via cpupstate_set_state().
 */
static void ondemand_evaluate_cpu(struct od_cpu_state *state)
{
    int num_states = cpupstate_get_count();
    if (num_states <= 0)
        return;  /* No P-states available — nothing to do */

    int current_state = cpupstate_get_state();
    if (current_state < 0 || current_state >= num_states)
        current_state = 0;

    /* Compute current load */
    int load = compute_load(state);
    state->current_load = load;

    /* Read current timer ticks for rate-limiting */
    uint64_t now = timer_get_ticks();

    /* ── Scale-up decision ─────────────────────────────────────────
     * If load exceeds the up-threshold, scale up one P-state.
     * Rate-limited: at most one up-transition per sampling period. */
    if (load >= g_up_threshold) {
        if (current_state > 0 &&
            (int64_t)(now - state->last_up_tick) >= g_sampling_rate) {
            int new_state = current_state - 1;  /* lower number = higher freq */
            if (new_state < 0) new_state = 0;

            if (new_state != current_state) {
                cpupstate_set_state(new_state);
                state->last_up_tick = now;
                kprintf("[ondemand] scale UP: load=%d%% -> P%d (freq=%u MHz)\n",
                        load, new_state, pstate_freq_mhz(new_state));
            }
        }
        state->low_load_count = 0;  /* Reset low-load counter */
        return;
    }

    /* ── Scale-down decision ───────────────────────────────────────
     * If load is below the down-threshold, we wait for several
     * consecutive low-load samples before scaling down.  This prevents
     * oscillation when load hovers near the boundary. */
    if (load <= g_down_threshold) {
        state->low_load_count++;
    } else {
        state->low_load_count = 0;
        return;  /* Load in the "hold" zone — maintain current frequency */
    }

    /* Scale down only after hysteresis count */
    if (state->low_load_count >= ONDEMAND_DOWN_HYSTERESIS &&
        current_state < num_states - 1 &&
        (int64_t)(now - state->last_down_tick) >= g_sampling_rate) {
        int new_state = current_state + 1;  /* higher number = lower freq */
        if (new_state >= num_states) new_state = num_states - 1;

        if (new_state != current_state) {
            cpupstate_set_state(new_state);
            state->last_down_tick = now;
            kprintf("[ondemand] scale DOWN: load=%d%% -> P%d (freq=%u MHz)\n",
                    load, new_state, pstate_freq_mhz(new_state));
        }
        state->low_load_count = 0;  /* Reset after scaling */
    }
}

/* ── Timer callback ─────────────────────────────────────────────────
 * Called periodically (every g_sampling_rate ticks) to evaluate
 * the CPU load and adjust frequency.  Reschedules itself.
 */
static void ondemand_timer_cb(void *arg)
{
    (void)arg;
    if (!g_running) return;

    /* Evaluate for the current CPU */
    struct od_cpu_state *state = this_od_state();
    ondemand_evaluate_cpu(state);

    /* Re-schedule the next sample */
    g_timer_id = timer_schedule(ondemand_timer_cb, NULL, (uint64_t)g_sampling_rate);
}

/* ── Public API ───────────────────────────────────────────────────── */

int cpufreq_ondemand_init(void)
{
    if (!cpupstate_is_present()) {
        kprintf("[ondemand] CPU freq scaling not present — disabled\n");
        return -1;
    }

    /* Initialize per-CPU state */
    for (int i = 0; i < OD_MAX_CPUS; i++) {
        od_state[i].last_up_tick    = 0;
        od_state[i].last_down_tick  = 0;
        od_state[i].saved_mperf     = read_msr(MSR_IA32_MPERF);
        od_state[i].saved_aperf     = read_msr(MSR_IA32_APERF);
        od_state[i].low_load_count  = 0;
        od_state[i].current_load    = 0;
    }

    kprintf("[ondemand] Governor initialized (rate=%d ticks, up=%d%%, down=%d%%)\n",
            g_sampling_rate, g_up_threshold, g_down_threshold);
    return 0;
}

int cpufreq_ondemand_start(void)
{
    if (g_running) return 0;   /* Already running */
    if (!cpupstate_is_present()) return -1;

    g_running = 1;

    /* Schedule the first sample immediately */
    g_timer_id = timer_schedule(ondemand_timer_cb, NULL, 1); /* fire in 1 tick */

    kprintf("[ondemand] Sampling started (every %d ticks)\n", g_sampling_rate);
    return 0;
}

void cpufreq_ondemand_stop(void)
{
    if (!g_running) return;
    g_running = 0;

    if (g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = -1;
    }

    kprintf("[ondemand] Sampling stopped\n");
}

/* Immediate evaluation (useful when switching to ondemand governor) */
void cpufreq_ondemand_evaluate(void)
{
    if (!g_running || !cpupstate_is_present()) return;
    struct od_cpu_state *state = this_od_state();
    if (state)
        ondemand_evaluate_cpu(state);
}

/* ── od_speed_up ─────────────────────────────── */
int od_speed_up(int cpu)
{
    (void)cpu;
    int cur = cpupstate_get_state();
    if (cur <= 0) return 0;
    return cpupstate_set_state(cur - 1);
}
/* ── od_slow_down ─────────────────────────────── */
int od_slow_down(int cpu)
{
    (void)cpu;
    int cur = cpupstate_get_state();
    int max = cpupstate_get_count();
    if (cur < 0 || cur >= max - 1) return 0;
    return cpupstate_set_state(cur + 1);
}
/* ── od_target ─────────────────────────────── */
int od_target(int cpu, unsigned int target_freq)
{
    (void)cpu;
    int count = cpupstate_get_count();
    int best = 0;
    for (int i = 0; i < count; i++) {
        struct cpupstate_state info;
        if (cpupstate_get_info(i, &info) == 0) {
            if (info.core_freq * 1000 <= target_freq)
                best = i;
        }
    }
    return cpupstate_set_state(best);
}
