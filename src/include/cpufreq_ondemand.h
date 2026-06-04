#ifndef CPUFREQ_ONDEMAND_H
#define CPUFREQ_ONDEMAND_H

#include "types.h"

/*
 * cpufreq_ondemand.h — OnDemand CPU frequency governor interface
 *
 * The ondemand governor periodically samples CPU utilization and adjusts
 * the P-state (frequency/voltage) based on load:
 *   - Load > up_threshold   → scale up (higher frequency)
 *   - Load < down_threshold → scale down (lower frequency)
 *   - Between thresholds    → maintain current frequency
 *
 * Transition rates are limited to prevent thrashing.
 *
 * Changes are driven by the cpupstate API (cpufreq.c) which writes MSRs.
 */

/* ── Tunable parameters (writable via sysfs) ───────────────────────── */

/* Sampling interval in timer ticks (default: 10 ticks = 100 ms at 100 Hz) */
#define ONDEMAND_SAMPLING_RATE_DEFAULT  10

/* CPU load percentage above which we scale up (default: 80%) */
#define ONDEMAND_UP_THRESHOLD_DEFAULT   80

/* CPU load percentage below which we scale down (default: 20%) */
#define ONDEMAND_DOWN_THRESHOLD_DEFAULT 20

/* Minimum ticks between frequency transitions (rate limit, default: 2) */
#define ONDEMAND_MIN_SAMPLE_RATE        2

/* ── External interface ───────────────────────────────────────────── */

/* Initialize the ondemand governor (called from cpufreq_init or separately) */
int cpufreq_ondemand_init(void);

/* Start periodic sampling (must be called after init) */
int cpufreq_ondemand_start(void);

/* Stop periodic sampling */
void cpufreq_ondemand_stop(void);

/* Re-evaluate target frequency immediately (one-shot) */
void cpufreq_ondemand_evaluate(void);

/* Return 1 if ondemand governor is actively sampling */
int cpufreq_ondemand_is_active(void);

/* Accessors for sysfs tunable parameters */
int  ondemand_get_sampling_rate(void);
int  ondemand_set_sampling_rate(int ticks);
int  ondemand_get_up_threshold(void);
int  ondemand_set_up_threshold(int pct);
int  ondemand_get_down_threshold(void);
int  ondemand_set_down_threshold(int pct);
int  ondemand_get_ignore_nice(void);
void ondemand_set_ignore_nice(int val);

#endif /* CPUFREQ_ONDEMAND_H */
