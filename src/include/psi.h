#ifndef PSI_H
#define PSI_H

/*
 * psi.h — Pressure Stall Information (PSI)
 *
 * Tracks resource stall times for CPU, memory, and I/O, and computes
 * exponential moving averages for 10s, 60s, and 300s windows.  Exposed
 * via /proc/pressure/{cpu,memory,io} in the Linux format:
 *
 *   some avg10=0.00 avg60=0.00 avg300=0.00 total=0
 *   full avg10=0.00 avg60=0.00 avg300=0.00 total=0
 *
 * The "some" line shows the fraction of time *at least one* task was
 * stalled on this resource.  The "full" line shows the fraction of
 * time *all* non-idle tasks were stalled simultaneously.
 *
 * Reference: https://www.kernel.org/doc/html/latest/accounting/psi.html
 */

#include "types.h"

/* ── Resource types ────────────────────────────────────────────────── */
#define PSI_RES_CPU    0
#define PSI_RES_MEMORY 1
#define PSI_RES_IO     2
#define PSI_NUM_RESOURCES 3

/* ── Averaging windows (in seconds) ────────────────────────────────── */
#define PSI_WINDOW_10S   10
#define PSI_WINDOW_60S   60
#define PSI_WINDOW_300S  300
#define PSI_NUM_WINDOWS  3

/* ── Stall-state flags (per task) ──────────────────────────────────── */
#define PSI_FLAG_CPU_STALL     (1U << 0)
#define PSI_FLAG_MEM_STALL    (1U << 1)
#define PSI_FLAG_IO_STALL     (1U << 2)

/* ── Exported API ──────────────────────────────────────────────────── */

/* Initialise PSI tracking.  Called once during boot. */
void psi_init(void);

/* Initialise the periodic PSI update timer.  Called after timers_init(). */
void psi_timer_init(void);

/*
 * Update stall tracking for a given resource.
 *
 * Called from the scheduler tick and other periodic sources.
 * @resource: PSI_RES_CPU, PSI_RES_MEMORY, or PSI_RES_IO
 * @wall_ticks:  number of timer ticks elapsed since last update
 * @some_ticks:  ticks where at least one task was stalled
 * @full_ticks:  ticks where all non-idle tasks were stalled
 */
void psi_update(int resource, uint64_t wall_ticks,
                uint64_t some_ticks, uint64_t full_ticks);

/*
 * Generate the contents of /proc/pressure/<resource>.
 *
 * Writes to @buf (up to @max bytes) and returns the length written,
 * or negative on error.
 */
int psi_gen_proc_file(int resource, char *buf, int max);

/*
 * ── Per-resource stall begin / end markers ───────────────────────────
 *
 * These track concurrency: the number of tasks currently stalled on a
 * resource.  psi_update() uses these counts to determine "some" (≥1)
 * vs "full" (all non-idle tasks stalled) pressure state.
 *
 * psi_cpu_enter / leave:
 *   Called from the scheduler on context switch.
 *   psi_cpu_enter() when a task becomes runnable but not running
 *     (preempted / yields while still runnable).
 *   psi_cpu_leave() when a task acquires the CPU (is scheduled in).
 *
 * psi_memstall_enter / leave:
 *   Wrap page-fault handling and any other synchronous memory wait.
 *   psi_memstall_enter() at fault entry, psi_memstall_leave() at exit.
 *
 * psi_io_enter / leave:
 *   Wrap IO submission / completion in the block layer.
 */
void psi_cpu_enter(void);
void psi_cpu_leave(void);
void psi_memstall_enter(void);
void psi_memstall_leave(void);
void psi_io_enter(void);
void psi_io_leave(void);

/*
 * Read the current number of tasks stalled on a given resource.
 * Used internally and for diagnostics (e.g. /proc/pressure).
 */
int psi_stall_count(int resource);

#endif /* PSI_H */
