/* nohz.c — NO_HZ_FULL (adaptive tick) for isolated CPUs
 *
 * Implements adaptive tick infrastructure:
 *   - nohz_init() — initialise per-CPU tick state
 *   - nohz_tick_stop() — stop the periodic tick on an isolated CPU
 *   - nohz_tick_restart() — restart the tick on a CPU
 *
 * When a CPU is isolated (NO_HZ_FULL), the periodic timer interrupt
 * is suppressed to reduce noise. The tick is re-enabled when:
 *   1. More than one task is runnable on that CPU
 *   2. A kernel task needs precise timing
 *   3. RCU needs a grace period tick
 *
 * This is a basic stub suitable for an educational kernel.
 */

#include "nohz.h"
#include "smp.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "errno.h"

/* ── Per-CPU NO_HZ state ─────────────────────────────────────────── */

struct nohz_state {
    int  in_use;
    int  tick_stopped;          /* 1 if periodic tick is stopped */
    int  isolated;              /* 1 if CPU is isolated (adaptive tick) */
    uint64_t tick_stopped_at;   /* timestamp when tick was stopped */
    uint64_t last_tick;         /* last tick timestamp */
};

#define NOHZ_MAX_CPUS 64
static struct nohz_state nohz_cpus[NOHZ_MAX_CPUS];
static int nohz_initialised = 0;

/* ── Public API ──────────────────────────────────────────────────── */

void nohz_init(void)
{
    if (nohz_initialised)
        return;

    memset(nohz_cpus, 0, sizeof(nohz_cpus));
    nohz_initialised = 1;
    kprintf("[NOHZ] Adaptive tick infrastructure initialised\n");
}

/* Mark a CPU as isolated (candidate for adaptive tick). */
int nohz_isolate_cpu(int cpu)
{
    if (!nohz_initialised)
        return -EPERM;
    if (cpu < 0 || cpu >= NOHZ_MAX_CPUS)
        return -EINVAL;

    nohz_cpus[cpu].in_use = 1;
    nohz_cpus[cpu].isolated = 1;
    nohz_cpus[cpu].tick_stopped = 0;
    kprintf("[NOHZ] CPU %d marked isolated\n", cpu);
    return 0;
}

/* Stop the periodic tick on a given CPU.
 * Called from the timer interrupt handler when the CPU is idle
 * and no tasks require a periodic tick.
 * Returns 0 on success. */
int nohz_tick_stop(int cpu)
{
    if (!nohz_initialised)
        return -EPERM;
    if (cpu < 0 || cpu >= NOHZ_MAX_CPUS || !nohz_cpus[cpu].in_use)
        return -EINVAL;
    if (!nohz_cpus[cpu].isolated)
        return -EINVAL;  /* only isolated CPUs can stop tick */

    if (nohz_cpus[cpu].tick_stopped)
        return 0;  /* already stopped */

    nohz_cpus[cpu].tick_stopped = 1;
    nohz_cpus[cpu].tick_stopped_at = timer_get_ms();
    kprintf("[NOHZ] Periodic tick stopped on CPU %d\n", cpu);
    return 0;
}

/* Restart the periodic tick on a given CPU.
 * Called when a new task becomes runnable or when RCU needs a tick.
 * Returns 0 on success. */
int nohz_tick_restart(int cpu)
{
    if (!nohz_initialised)
        return -EPERM;
    if (cpu < 0 || cpu >= NOHZ_MAX_CPUS || !nohz_cpus[cpu].in_use)
        return -EINVAL;

    if (!nohz_cpus[cpu].tick_stopped)
        return 0;  /* already running */

    nohz_cpus[cpu].tick_stopped = 0;
    kprintf("[NOHZ] Periodic tick restarted on CPU %d\n", cpu);
    return 0;
}

/* Check whether the tick is stopped on a given CPU.
 * Returns 1 if stopped, 0 if running. */
int nohz_tick_is_stopped(int cpu)
{
    if (!nohz_initialised || cpu < 0 || cpu >= NOHZ_MAX_CPUS)
        return 0;
    return nohz_cpus[cpu].tick_stopped;
}

/* Check whether a given CPU is marked for NO_HZ adaptive tick. */
int nohz_cpu_is_isolated(int cpu)
{
    if (!nohz_initialised || cpu < 0 || cpu >= NOHZ_MAX_CPUS)
        return 0;
    return nohz_cpus[cpu].isolated;
}

/* Return the number of milliseconds since the tick was stopped. */
uint64_t nohz_tick_stopped_ms(int cpu)
{
    if (!nohz_initialised || cpu < 0 || cpu >= NOHZ_MAX_CPUS)
        return 0;
    if (!nohz_cpus[cpu].tick_stopped)
        return 0;
    return timer_get_ms() - nohz_cpus[cpu].tick_stopped_at;
}

/* Update last tick timestamp (called from timer interrupt). */
void nohz_tick_account(int cpu)
{
    if (!nohz_initialised || cpu < 0 || cpu >= NOHZ_MAX_CPUS)
        return;
    nohz_cpus[cpu].last_tick = timer_get_ms();
}

/* ── Stub: nohz_full_setup ──────────────────────────────────────────────── */
int nohz_full_setup(const char *cpulist)
{
    (void)cpulist;
    kprintf("[NOHZ] nohz_full_setup not yet implemented\n");
    return 0;
}

/* ── Stub: nohz_full_kick ──────────────────────────────────────────────── */
void nohz_full_kick(int cpu)
{
    (void)cpu;
    kprintf("[NOHZ] nohz_full_kick not yet implemented\n");
}
