#ifndef CPUFREQ_SCHEDUTIL_H
#define CPUFREQ_SCHEDUTIL_H

/*
 * cpufreq_schedutil.h — SchedUtil CPU frequency scaling governor
 *
 * Reads task utilization directly from PELT (Per-Entity Load Tracking)
 * and adjusts the P-state to match workload demand.  Responds faster
 * than ondemand because it uses instantaneous scheduler utilization
 * rather than APERF/MPERF sampling.
 *
 * Item 105 — CPU frequency: schedutil governor
 */

#include "types.h"

/* ── Tunable defaults ────────────────────────────────────────────── */

/* Default sampling rate in timer ticks (5 ticks = 50 ms at 100 Hz) */
#define SCHEDUTIL_SAMPLING_RATE_DEFAULT   5

/* Utilization thresholds (scaled to PELT_SCALE=1024).
 * util_avg runs 0..1024; these thresholds define when to scale. */
#define SCHEDUTIL_UP_THRESHOLD_DEFAULT    768   /* ~75% · PELT_SCALE */
#define SCHEDUTIL_DOWN_THRESHOLD_DEFAULT  256   /* ~25% · PELT_SCALE */

/* ── Public API ──────────────────────────────────────────────────── */

/* Initialise the schedutil governor (must be called once at boot) */
int  cpufreq_schedutil_init(void);

/* Start/stop periodic sampling */
int  cpufreq_schedutil_start(void);
void cpufreq_schedutil_stop(void);

/* Query whether the governor is active */
int  cpufreq_schedutil_is_active(void);

/* Immediate evaluation using current PELT values.
 * Called when a task is enqueued / migrated to trigger instant
 * frequency scaling without waiting for the next timer tick. */
void cpufreq_schedutil_evaluate(void);

/* Called from the scheduler when a task's PELT is updated.
 * @cpu_id:   CPU where the task is running
 * @util_avg: current util_avg value (0..PELT_SCALE=1024)
 * This allows the governor to react instantly to utilization changes. */
void cpufreq_schedutil_pelt_update(int cpu_id, uint32_t util_avg);

/* Tunable accessors (for sysfs integration) */
int  schedutil_get_sampling_rate(void);
int  schedutil_set_sampling_rate(int ticks);
int  schedutil_get_up_threshold(void);
int  schedutil_set_up_threshold(int pct);
int  schedutil_get_down_threshold(void);
int  schedutil_set_down_threshold(int pct);

#endif /* CPUFREQ_SCHEDUTIL_H */
