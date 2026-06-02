#ifndef PELT_H
#define PELT_H

#include "types.h"

/*
 * PELT — Per-Entity Load Tracking
 *
 * Tracks a decaying average of each task's CPU utilization using an
 * exponential weighted moving average (EWMA) with a half-life of ~32
 * timer ticks (~32 ms at 1 kHz tick rate).
 *
 * Two metrics are maintained per task:
 *   util_avg  — average of actual CPU running time (0..1024, scaled)
 *   load_avg  — average of runnable state (running + waiting, 0..1024)
 *
 * The decay factor is configurable via PELT_HALFLIFE.
 *
 * Each metric uses the formula:
 *   avg = (avg * (2^SHIFT - 1) + new * 1) / 2^SHIFT
 * where 2^SHIFT = PELT_HALFLIFE, giving a half-life of ~PELT_HALFLIFE ticks.
 */

/* ── PELT constants ────────────────────────────────────────────── */

/* Half-life in timer ticks (32 ticks ≈ 32 ms at 1 kHz timer) */
#define PELT_HALFLIFE           32

/* Scale factor for fixed-point representation (0..LOAD_AVG_MAX) */
#define PELT_SCALE              1024

/* Initial load on creation (avoids zero-weight issues for short-lived tasks) */
#define PELT_INIT_LOAD          256   /* ~25% of max load as starting value */

/* Maximum load average value (full utilization) */
#define LOAD_AVG_MAX            PELT_SCALE

/* ── Per-task PELT state ───────────────────────────────────────── */

struct pelt_state {
    uint32_t util_avg;      /* running average * PELT_SCALE (0..PELT_SCALE) */
    uint32_t load_avg;      /* runnable average * PELT_SCALE (0..PELT_SCALE) */
    uint32_t last_update;   /* timer tick of last PELT update */
};

/* ── Public API ────────────────────────────────────────────────── */

/* Initialise PELT tracking for a process */
void pelt_init(struct pelt_state *pelt);

/* Update PELT after a timer tick.
 * @running: 1 if the task was running (on CPU), 0 otherwise.
 * @runnable: 1 if the task was runnable (on runqueue), 0 otherwise.
 * @now: current timer tick count.
 */
void pelt_update(struct pelt_state *pelt, int running, int runnable,
                 uint64_t now);

/* Decay an average by (n) missed ticks (for sleeping/waiting tasks).
 * Called when a task has not been updated for several ticks.
 */
void pelt_decay_missed(struct pelt_state *pelt, unsigned int n_ticks);

/* Subsystem initialisation (prints status message) */
void pelt_subsys_init(void);

#endif /* PELT_H */
