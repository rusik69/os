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

/* ── Exported API ──────────────────────────────────────────────────── */

/* Initialise PSI tracking.  Called once during boot. */
void psi_init(void);

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

#endif /* PSI_H */
