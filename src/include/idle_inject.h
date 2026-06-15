#ifndef IDLE_INJECT_H
#define IDLE_INJECT_H

#include "types.h"

/*
 * Idle injection — force a CPU into idle for thermal/power management.
 *
 * When a CPU core gets too hot (or the platform requests power capping),
 * idle injection forces the CPU to stop executing for a configurable
 * fraction of a duty cycle by executing HLT/MWAIT.
 *
 * The duty cycle is controlled by two parameters:
 *   - run_duration:  time (in ms) the CPU runs normally
 *   - idle_duration: time (in ms) the CPU is forced into idle
 *
 * The idle period uses the architecture-specific idle instruction (HLT
 * or MWAIT) via the cpuidle subsystem.  A per-CPU timer fires to
 * switch between run and idle states.
 */

/* Maximum number of CPUs that can have idle injection registered */
#define IDLE_INJECT_MAX_CPUS 64

/* ── Idle injection control ────────────────────────────────────────── */

/*
 * Register idle injection on a CPU.
 * @cpu:       Target CPU number
 * @max_ratio: Maximum idle ratio (0-100, 0 = no injection, 100 = always idle)
 * @duration:  Base cycle duration in ms (determines timer period)
 *
 * Returns 0 on success, -errno on error.
 */
int idle_inject_register(int cpu, unsigned int max_ratio, unsigned int duration);

/*
 * Unregister idle injection on a CPU.
 * @cpu: Target CPU number
 *
 * Returns 0 on success, -errno on error.
 */
int idle_inject_unregister(int cpu);

/*
 * Set the run/idle duty cycle for a CPU.
 * @cpu:    Target CPU number
 * @run:    Run duration in ms
 * @idle:   Idle duration in ms
 *
 * Returns 0 on success, -errno on error.
 */
int idle_inject_set_duration(int cpu, unsigned int run, unsigned int idle);

/* ── Initialisation ────────────────────────────────────────────────── */

/* Initialise the idle injection subsystem. */
void idle_inject_init(void);

#ifdef KERNEL_INTERNAL

/*
 * Timer tick handler — called from the scheduler tick or a dedicated
 * timer.  Checks if any CPU has an idle injection cycle pending and
 * forces idle if needed.
 */
void idle_inject_tick(int cpu);

/* Check if the current CPU is inside an injected idle period.
 * Returns 1 if idle should be injected, 0 otherwise. */
int idle_inject_is_idle(int cpu);

#endif /* KERNEL_INTERNAL */

#endif /* IDLE_INJECT_H */
