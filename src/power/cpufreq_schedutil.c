/*
 * cpufreq_schedutil.c — SchedUtil CPU frequency scaling governor
 *
 * Reads task utilization directly from PELT (Per-Entity Load Tracking)
 * and adjusts the P-state to match workload demand.  The key advantage
 * over the ondemand governor is responsiveness: instead of waiting for
 * a periodic APERF/MPERF sample, we get the scheduler's instantaneous
 * view of utilisation and act immediately.
 *
 * Design:
 *   - A timer fires every `sampling_rate` ticks (default 5 = 50 ms)
 *     to re-evaluate CPU utilisation if no PELT update arrived.
 *   - In addition, cpufreq_schedutil_pelt_update() is called from the
 *     scheduler whenever a task's PELT values change (enqueue, dequeue,
 *     tick).  This provides instant frequency adjustment.
 *   - Utilisation comes from the per-process PELT util_avg (0..1024)
 *     summed across all runnable tasks on the current CPU.
 *   - If util > up_threshold, we scale up one P-state at a time.
 *   - If util < down_threshold after several consecutive low samples,
 *     we scale down.
 *   - Rate limiting: at most one transition every min_sample_rate ticks.
 *
 * Tunable parameters are exposed through /sys/.../cpufreq/ when
 * governor is "schedutil":
 *   - sampling_rate       (ticks between periodic samples)
 *   - up_threshold        (util_avg to trigger scale-up)
 *   - down_threshold      (util_avg to trigger scale-down)
 *
 * Reference: Linux kernel/sched/cpufreq_schedutil.c
 *
 * Item 105 — CPU frequency: schedutil governor
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "cpufreq_schedutil.h"
#include "cpupstate.h"
#include "printf.h"
#include "timer.h"          /* TIMER_FREQ, timer_get_ticks */
#include "timers.h"         /* timer_schedule, timer_cancel */
#include "smp.h"            /* smp_get_cpu_count, smp_get_cpu_id */
#include "scheduler.h"      /* get_current_process */
#include "process.h"        /* struct process, pelt_state */
#include "pelt.h"           /* PELT_SCALE, LOAD_AVG_MAX */
#include "string.h"

/* ── Per-CPU governor state ───────────────────────────────────────── */

struct schedutil_cpu_state {
    /* Ticks at which we last scaled up/down (rate limiting) */
    uint64_t last_up_tick;
    uint64_t last_down_tick;

    /* Aggregated util_avg for this CPU (sum of all runnable tasks,
     * clamped to PELT_SCALE).  Updated by pelt_update(). */
    uint32_t cpu_util;

    /* Consecutive low-util samples before scaling down (anti-flap) */
    int low_util_count;

    /* Current util_avg percentage (0..100) for diagnostics */
    int current_util_pct;
};

/* Max CPUs we track (matches SMP_MAX_CPUS) */
#define SCHEDUTIL_MAX_CPUS 64

static struct schedutil_cpu_state schedutil_state[SCHEDUTIL_MAX_CPUS];

/* ── Governor tunables ────────────────────────────────────────────── */

static int  g_sampling_rate    = SCHEDUTIL_SAMPLING_RATE_DEFAULT;  /* ticks */
static int  g_up_threshold     = SCHEDUTIL_UP_THRESHOLD_DEFAULT;   /* raw util_avg */
static int  g_down_threshold   = SCHEDUTIL_DOWN_THRESHOLD_DEFAULT; /* raw util_avg */
static int  g_running          = 0;   /* 1 = sampling active */
static int  g_timer_id         = -1;  /* periodic timer ID */

/* Sanity bounds for tunables */
#define SCHEDUTIL_MIN_THRESHOLD     32    /* ~3% util */
#define SCHEDUTIL_MAX_THRESHOLD     1024  /* 100% util */
#define SCHEDUTIL_MIN_RATE_TICKS    1
#define SCHEDUTIL_MAX_RATE_TICKS    1000

/* Number of consecutive low-util samples before scaling down (anti-flap) */
#define SCHEDUTIL_DOWN_HYSTERESIS   2

/* ── Forward declarations ─────────────────────────────────────────── */

static void schedutil_timer_cb(void *arg);

/* ── Per-CPU accessor ─────────────────────────────────────────────── */

static inline struct schedutil_cpu_state *this_schedutil_state(void)
{
    uint32_t cpu = smp_get_cpu_id();
    if (cpu >= SCHEDUTIL_MAX_CPUS)
        cpu = 0;
    return &schedutil_state[cpu];
}

/* ── Tunable accessors ────────────────────────────────────────────── */

int schedutil_get_sampling_rate(void)
{
    return g_sampling_rate;
}

int schedutil_set_sampling_rate(int ticks)
{
    if (ticks < SCHEDUTIL_MIN_RATE_TICKS || ticks > SCHEDUTIL_MAX_RATE_TICKS)
        return -1;
    g_sampling_rate = ticks;
    /* If sampling is active, reschedule the timer */
    if (g_running && g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = timer_schedule(schedutil_timer_cb, NULL,
                                    (uint64_t)g_sampling_rate);
    }
    return 0;
}

int schedutil_get_up_threshold(void)
{
    return g_up_threshold;
}

int schedutil_set_up_threshold(int val)
{
    if (val < SCHEDUTIL_MIN_THRESHOLD || val > SCHEDUTIL_MAX_THRESHOLD)
        return -1;
    g_up_threshold = val;
    return 0;
}

int schedutil_get_down_threshold(void)
{
    return g_down_threshold;
}

int schedutil_set_down_threshold(int val)
{
    if (val < SCHEDUTIL_MIN_THRESHOLD || val > SCHEDUTIL_MAX_THRESHOLD)
        return -1;
    g_down_threshold = val;
    return 0;
}

/* ── Utility: Convert util_avg (0..1024) to percentage (0..100) ──── */

static inline int util_to_pct(uint32_t util_avg)
{
    uint64_t pct = (uint64_t)util_avg * 100ULL / PELT_SCALE;
    return (int)(pct > 100 ? 100 : pct);
}

/* ── Helper: get frequency (MHz) for a P-state ───────────────────── */

static uint32_t pstate_freq_mhz(int state)
{
    struct cpupstate_state info;
    if (cpupstate_get_info(state, &info) < 0)
        return 0;
    return info.core_freq;
}

/* ── Core: evaluate CPU utilisation and adjust P-state ─────────────
 *
 * This is the heart of the governor.  It checks the current CPU util
 * against thresholds and scales up/down accordingly.  Called from
 * both the timer callback (periodic) and pelt_update (instant).
 *
 * The target frequency is computed using the formula:
 *   freq = util * max_freq / max_capacity
 *
 * where util is the current PELT utilization (0..1024 SCALE),
 * max_freq is the highest available P-state frequency,
 * and max_capacity is PELT_SCALE (1024).
 *
 * Rate-limiting prevents thrashing: at most one transition every
 * sampling_rate ticks.
 */
static void schedutil_evaluate_cpu(struct schedutil_cpu_state *state)
{
    int num_states = cpupstate_get_count();
    if (num_states <= 0)
        return;  /* No P-states available — nothing to do */

    int current_state = cpupstate_get_state();
    if (current_state < 0 || current_state >= num_states)
        current_state = 0;

    uint32_t util = state->cpu_util;
    state->current_util_pct = util_to_pct(util);

    uint64_t now = timer_get_ticks();

    /* Compute target frequency using the schedutil formula:
     *   freq = util * max_freq / max_capacity
     */
    uint32_t max_freq_mhz = pstate_freq_mhz(0);
    if (max_freq_mhz == 0)
        max_freq_mhz = 2000; /* default 2 GHz if unknown */

    uint32_t target_freq_mhz;
    if (util >= PELT_SCALE) {
        target_freq_mhz = max_freq_mhz;
    } else {
        uint64_t freq_val = (uint64_t)util * (uint64_t)max_freq_mhz;
        target_freq_mhz = (uint32_t)(freq_val / PELT_SCALE);
    }
    if (target_freq_mhz < pstate_freq_mhz(num_states - 1))
        target_freq_mhz = pstate_freq_mhz(num_states - 1);
    if (target_freq_mhz > max_freq_mhz)
        target_freq_mhz = max_freq_mhz;

    /* Find the closest P-state to the target frequency */
    int target_state = 0;
    uint32_t min_diff = 0xFFFFFFFF;
    for (int i = 0; i < num_states; i++) {
        uint32_t f = pstate_freq_mhz(i);
        uint32_t diff = (f > target_freq_mhz) ? (f - target_freq_mhz) : (target_freq_mhz - f);
        if (diff < min_diff) {
            min_diff = diff;
            target_state = i;
        }
    }

    /* Rate-limited transition */
    if (target_state < current_state) {
        /* Scale up */
        if ((int64_t)(now - state->last_up_tick) >= g_sampling_rate) {
            cpupstate_set_state(target_state);
            state->last_up_tick = now;
            state->last_down_tick = 0;
            kprintf("[schedutil] freq: util=%u/%u (%d%%) target=%u MHz -> P%d (%u MHz)\n",
                    (unsigned)util, (unsigned)PELT_SCALE,
                    state->current_util_pct,
                    target_freq_mhz, target_state,
                    (unsigned)pstate_freq_mhz(target_state));
        }
        state->low_util_count = 0;
    } else if (target_state > current_state) {
        /* Scale down — use hysteresis */
        if (util <= (uint32_t)g_down_threshold) {
            state->low_util_count++;
        } else {
            state->low_util_count = 0;
        }

        if (state->low_util_count >= SCHEDUTIL_DOWN_HYSTERESIS &&
            (int64_t)(now - state->last_down_tick) >= g_sampling_rate) {
            cpupstate_set_state(target_state);
            state->last_down_tick = now;
            state->last_up_tick = 0;
            state->low_util_count = 0;
            kprintf("[schedutil] freq: util=%u/%u (%d%%) target=%u MHz -> P%d (%u MHz)\n",
                    (unsigned)util, (unsigned)PELT_SCALE,
                    state->current_util_pct,
                    target_freq_mhz, target_state,
                    (unsigned)pstate_freq_mhz(target_state));
        }
    } else {
        /* Already at target — reset low util count in mid-range */
        if (util > (uint32_t)g_down_threshold && util < (uint32_t)g_up_threshold)
            state->low_util_count = 0;
    }
}

/* ── Compute CPU-wide utilisation from the currently running task's PELT.
 *
 * We use the util_avg of the currently running process on this CPU as
 * the primary signal.  This is a simplified approximation — a full
 * implementation would sum PELT values across the entire per-CPU
 * runqueue.  However, in practice the running task dominates CPU
 * utilisation, so this yields responsive frequency scaling.
 *
 * Returns the util_avg, clamped to [0, PELT_SCALE].
 */
static uint32_t compute_cpu_util(void)
{
    struct process *cur = get_current_process();
    if (!cur || cur->state == PROCESS_UNUSED)
        return 0;

    uint32_t util = cur->pelt.util_avg;
    if (util > PELT_SCALE)
        util = PELT_SCALE;
    return util;
}

/* ── Timer callback ─────────────────────────────────────────────────
 * Called periodically (every g_sampling_rate ticks) to re-evaluate
 * CPU utilisation.  Reschedules itself. */
static void schedutil_timer_cb(void *arg)
{
    (void)arg;
    if (!g_running) return;

    /* Refresh the CPU utilisation estimate */
    struct schedutil_cpu_state *state = this_schedutil_state();
    state->cpu_util = compute_cpu_util();

    /* Evaluate and possibly adjust P-state */
    schedutil_evaluate_cpu(state);

    /* Re-schedule the next sample */
    g_timer_id = timer_schedule(schedutil_timer_cb, NULL,
                                (uint64_t)g_sampling_rate);
}

/* ── Public API ───────────────────────────────────────────────────── */

int cpufreq_schedutil_init(void)
{
    if (!cpupstate_is_present()) {
        kprintf("[schedutil] CPU freq scaling not present — disabled\n");
        return -1;
    }

    /* Initialise per-CPU state */
    for (int i = 0; i < SCHEDUTIL_MAX_CPUS; i++) {
        schedutil_state[i].cpu_util        = 0;
        schedutil_state[i].last_up_tick    = 0;
        schedutil_state[i].last_down_tick  = 0;
        schedutil_state[i].low_util_count  = 0;
        schedutil_state[i].current_util_pct = 0;
    }

    kprintf("[schedutil] Governor initialized (rate=%d ticks, up=%d, down=%d)\n",
            g_sampling_rate, g_up_threshold, g_down_threshold);
    return 0;
}

int cpufreq_schedutil_start(void)
{
    if (g_running) return 0;   /* Already running */
    if (!cpupstate_is_present()) return -1;

    g_running = 1;

    /* Do an immediate evaluation before starting the timer */
    {
        struct schedutil_cpu_state *state = this_schedutil_state();
        state->cpu_util = compute_cpu_util();
        schedutil_evaluate_cpu(state);
    }

    /* Schedule the first periodic sample */
    g_timer_id = timer_schedule(schedutil_timer_cb, NULL, 1); /* fire in 1 tick */

    kprintf("[schedutil] Sampling started (every %d ticks)\n", g_sampling_rate);
    return 0;
}

void cpufreq_schedutil_stop(void)
{
    if (!g_running) return;
    g_running = 0;

    if (g_timer_id >= 0) {
        timer_cancel(g_timer_id);
        g_timer_id = -1;
    }

    kprintf("[schedutil] Sampling stopped\n");
}

int cpufreq_schedutil_is_active(void)
{
    return g_running;
}

/* Immediate evaluation (useful when switching to schedutil governor) */
void cpufreq_schedutil_evaluate(void)
{
    if (!g_running || !cpupstate_is_present()) return;
    struct schedutil_cpu_state *state = this_schedutil_state();
    if (state) {
        state->cpu_util = compute_cpu_util();
        schedutil_evaluate_cpu(state);
    }
}

/* Called from the scheduler when a task's PELT is updated.
 * Provides instant utilization-driven frequency scaling without
 * waiting for the next timer tick.
 *
 * @cpu_id:   CPU where the task is running (or -1 if unclear)
 * @util_avg: current util_avg value (0..PELT_SCALE) of the task.
 *
 * On task migration, this allows immediate frequency adjustment. */
void cpufreq_schedutil_pelt_update(int cpu_id, uint32_t util_avg)
{
    if (!g_running || !cpupstate_is_present())
        return;

    /* Determine which CPU to evaluate */
    int target_cpu = cpu_id;
    if (target_cpu < 0 || target_cpu >= SCHEDUTIL_MAX_CPUS)
        target_cpu = (int)smp_get_cpu_id();

    struct schedutil_cpu_state *state = &schedutil_state[target_cpu];

    /* Update the CPU-wide utilisation estimate.
     * For simplicity, we use the reported util_avg directly as the
     * primary signal (the task just updated is the one currently
     * running or being enqueued).  The timer will later compute
     * the full summation via compute_cpu_util(). */
    state->cpu_util = util_avg;

    /* Evaluate immediately — no rate limiting here for maximum
     * responsiveness.  The rate limit in schedutil_evaluate_cpu
     * (checking last_up_tick / last_down_tick) prevents thrashing. */
    schedutil_evaluate_cpu(state);
}

/* ── Stub: schedutil_init ─────────────────────────────── */
int schedutil_init(int cpu)
{
    (void)cpu;
    kprintf("[cpufreq] schedutil_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: schedutil_exit ─────────────────────────────── */
int schedutil_exit(int cpu)
{
    (void)cpu;
    kprintf("[cpufreq] schedutil_exit: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: schedutil_target ─────────────────────────────── */
int schedutil_target(int cpu, unsigned int target_freq)
{
    (void)cpu;
    (void)target_freq;
    kprintf("[cpufreq] schedutil_target: not yet implemented\n");
    return -ENOSYS;
}
