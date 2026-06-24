/*
 * idle_inject.c — Force idle on a CPU for thermal/power management.
 *
 * When thermal throttling or power capping is needed, idle injection
 * forces a CPU to stop executing for a portion of a duty cycle.
 * The CPU enters architecture-specific idle (HLT or MWAIT) during
 * the idle phase.
 *
 * Design:
 *   - Per-CPU state machine: RUN phase → IDLE phase → RUN phase → ...
 *   - Timer-based switching: a per-CPU timer fires to transition
 *     between phases.
 *   - During the idle phase, the CPU executes HLT (via cpuidle).
 *   - Controls exposed via /sys/devices/system/cpu/cpuX/idle_inject/
 *
 * The duty cycle is defined by (run_ms, idle_ms) pairs.  The thermal
 * governor sets these dynamically based on temperature readings.
 */

#include "idle_inject.h"
#include "cpuidle.h"
#include "timer.h"
#include "spinlock.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── Per-CPU idle injection state ──────────────────────────────────── */

enum idle_inject_phase {
    IDLE_INJECT_PHASE_RUN,    /* CPU runs normally */
    IDLE_INJECT_PHASE_IDLE,   /* CPU forced into idle */
};

struct idle_inject_cpu {
    int registered;              /* 1 if idle injection is active on this CPU */
    unsigned int max_ratio;      /* Maximum idle ratio (0-100) */
    unsigned int run_ms;         /* Run duration in ms */
    unsigned int idle_ms;        /* Idle duration in ms */
    enum idle_inject_phase phase;/* Current phase */
    uint64_t phase_start_tick;   /* Timer tick when current phase started */
};

/* Per-CPU idle injection state array */
static struct idle_inject_cpu idle_inject_state[IDLE_INJECT_MAX_CPUS];

/* Global lock */
static spinlock_t ii_lock = SPINLOCK_INIT;

/* ── Internal helpers ───────────────────────────────────────────────── */

/* Get the state for a given CPU. Returns NULL if out of range. */
static struct idle_inject_cpu *ii_get_state(int cpu)
{
    if (cpu < 0 || cpu >= IDLE_INJECT_MAX_CPUS)
        return NULL;
    return &idle_inject_state[cpu];
}

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * idle_inject_register - Register a CPU for idle injection
 * @cpu: Logical CPU number to register
 * @max_ratio: Maximum idle ratio (0-100, percentage of time to idle)
 * @duration: Base duration of the duty cycle in milliseconds
 *
 * Enables idle injection on the specified CPU.  The run and idle phase
 * durations are derived from @max_ratio and @duration: idle_ms = duration
 * * max_ratio / 100, run_ms = duration - idle_ms.  Minimum durations
 * of 1 ms are enforced for both phases.
 *
 * Return: 0 on success, -EINVAL if @cpu is out of range or @max_ratio
 *         is invalid, -EBUSY if the CPU is already registered
 */
int idle_inject_register(int cpu, unsigned int max_ratio, unsigned int duration)
{
    struct idle_inject_cpu *state = ii_get_state(cpu);
    if (!state)
        return -EINVAL;

    if (max_ratio > 100)
        return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&ii_lock, &flags);

    if (state->registered) {
        spinlock_irqsave_release(&ii_lock, flags);
        return -EBUSY;
    }

    state->registered = 1;
    state->max_ratio = max_ratio;
    state->phase = IDLE_INJECT_PHASE_RUN;
    state->phase_start_tick = 0;

    /* Calculate run/idle durations from max_ratio and base duration.
     * idle_ms = duration * max_ratio / 100
     * run_ms  = duration - idle_ms */
    if (max_ratio >= 100) {
        state->run_ms = 0;
        state->idle_ms = duration;
    } else {
        state->idle_ms = (duration * max_ratio) / 100;
        state->run_ms = duration - state->idle_ms;
    }

    /* Ensure minimum durations */
    if (state->run_ms < 1 && max_ratio < 100)
        state->run_ms = 1;
    if (state->idle_ms < 1 && max_ratio > 0)
        state->idle_ms = 1;

    spinlock_irqsave_release(&ii_lock, flags);

    kprintf("[idle-inject] Registered CPU %d: run=%u ms, idle=%u ms (ratio=%u%%)\n",
            cpu, state->run_ms, state->idle_ms, max_ratio);

    return 0;
}

int idle_inject_unregister(int cpu)
{
    struct idle_inject_cpu *state = ii_get_state(cpu);
    if (!state)
        return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&ii_lock, &flags);

    if (!state->registered) {
        spinlock_irqsave_release(&ii_lock, flags);
        return -EINVAL;
    }

    state->registered = 0;
    state->max_ratio = 0;
    state->run_ms = 0;
    state->idle_ms = 0;
    state->phase = IDLE_INJECT_PHASE_RUN;

    spinlock_irqsave_release(&ii_lock, flags);

    kprintf("[idle-inject] Unregistered CPU %d\n", cpu);
    return 0;
}

int idle_inject_set_duration(int cpu, unsigned int run, unsigned int idle)
{
    struct idle_inject_cpu *state = ii_get_state(cpu);
    if (!state)
        return -EINVAL;

    uint64_t flags;
    spinlock_irqsave_acquire(&ii_lock, &flags);

    if (!state->registered) {
        spinlock_irqsave_release(&ii_lock, flags);
        return -EINVAL;
    }

    state->run_ms = run;
    state->idle_ms = idle;

    /* Recompute max_ratio from run/idle */
    unsigned int total = run + idle;
    if (total > 0)
        state->max_ratio = (idle * 100) / total;
    else
        state->max_ratio = 0;

    /* Reset phase to start a new cycle */
    state->phase = IDLE_INJECT_PHASE_RUN;
    state->phase_start_tick = 0;

    spinlock_irqsave_release(&ii_lock, flags);

    kprintf("[idle-inject] CPU %d: set run=%u ms, idle=%u ms (ratio=%u%%)\n",
            cpu, run, idle, state->max_ratio);

    return 0;
}

/* ── Timer tick handler ────────────────────────────────────────────── */

/**
 * idle_inject_tick - Timer-tick handler for idle injection state machine
 * @cpu: Logical CPU number to process
 *
 * Called from the scheduler or timer tick.  Checks if the current phase
 * (RUN or IDLE) has expired for the given CPU and transitions to the
 * next phase if needed.  The phase duration is converted from milliseconds
 * to ticks (assuming 100 Hz / 10 ms per tick).
 */
void idle_inject_tick(int cpu)
{
    struct idle_inject_cpu *state = ii_get_state(cpu);
    if (!state || !state->registered)
        return;

    uint64_t flags;
    spinlock_irqsave_acquire(&ii_lock, &flags);

    if (!state->registered) {
        spinlock_irqsave_release(&ii_lock, flags);
        return;
    }

    /* Check if current phase has expired.
     * We use timer ticks directly; assume 100 Hz timer = 10 ms per tick. */
    uint64_t now = 0;
    {
        /* Simple tick counter — use the system jiffies or a local counter */
        static uint64_t global_tick;
        now = ++global_tick;
    }

    if (state->phase_start_tick == 0)
        state->phase_start_tick = now;

    uint64_t elapsed_ticks = now - state->phase_start_tick;
    unsigned int phase_duration_ms;

    if (state->phase == IDLE_INJECT_PHASE_RUN)
        phase_duration_ms = state->run_ms;
    else
        phase_duration_ms = state->idle_ms;

    /* Convert ms to ticks (assuming 100 Hz → 10 ms/tick) */
    unsigned int phase_ticks = phase_duration_ms / 10;
    if (phase_ticks < 1)
        phase_ticks = 1;

    if (elapsed_ticks >= phase_ticks) {
        /* Transition to the next phase */
        if (state->phase == IDLE_INJECT_PHASE_RUN) {
            state->phase = IDLE_INJECT_PHASE_IDLE;
            kprintf("[idle-inject] CPU %d → IDLE phase (%u ms)\n",
                    cpu, state->idle_ms);
        } else {
            state->phase = IDLE_INJECT_PHASE_RUN;
            kprintf("[idle-inject] CPU %d → RUN phase (%u ms)\n",
                    cpu, state->run_ms);
        }
        state->phase_start_tick = now;
    }

    spinlock_irqsave_release(&ii_lock, flags);
}

/**
 * idle_inject_is_idle - Check if a CPU is currently in an injected idle phase
 * @cpu: Logical CPU number to query
 *
 * The scheduler calls this before picking a task.  If it returns 1,
 * the CPU should enter idle (HLT) instead of running any task.
 *
 * Return: 1 if the CPU is in an idle-injection idle phase, 0 otherwise
 */
int idle_inject_is_idle(int cpu)
{
    struct idle_inject_cpu *state = ii_get_state(cpu);
    if (!state || !state->registered)
        return 0;

    /* Fast path — check without lock since phase transitions are rare */
    return state->phase == IDLE_INJECT_PHASE_IDLE;
}

/* ── Initialisation ────────────────────────────────────────────────── */

/**
 * idle_inject_init - Initialise the idle injection subsystem
 *
 * Clears the per-CPU idle injection state array and initialises the
 * global spinlock.  Called once during boot before any CPU can be
 * registered for idle injection.
 */
void idle_inject_init(void)
{
    memset(idle_inject_state, 0, sizeof(idle_inject_state));
    spinlock_init(&ii_lock);

    kprintf("[idle-inject] Idle injection subsystem initialised (%d CPUs max)\n",
            IDLE_INJECT_MAX_CPUS);
}

/* ── Stub: idle_inject_set_rate ─────────────────────────────── */
int idle_inject_set_rate(int cpu, unsigned int rate)
{
    (void)cpu;
    (void)rate;
    kprintf("[idle] idle_inject_set_rate: not yet implemented\n");
    return 0;
}
