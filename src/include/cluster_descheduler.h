#ifndef CLUSTER_DESCHEDULER_H
#define CLUSTER_DESCHEDULER_H

#include "types.h"

/*
 * Cluster descheduler — evict pods from underutilised or overutilised
 * nodes and reschedule them for better packing.
 *
 * Policies:
 *   LowNodeUtilization  — evict from nodes below 30% CPU usage
 *   HighNodeUtilization — evict from nodes above 80% CPU usage
 */

/* ── Constants ──────────────────────────────────────────────────────── */

#define DESCHED_POLICY_NAME_MAX   64
#define DESCHED_POLICIES_MAX      16
#define DESCHED_NODE_NAME_MAX     128
#define DESCHED_EVICTIONS_MAX     64

/* ── Policy types ───────────────────────────────────────────────────── */

enum desched_policy_type {
    DESCHED_POLICY_LOW_UTIL    = 0,  /* < 30% CPU */
    DESCHED_POLICY_HIGH_UTIL   = 1,  /* > 80% CPU */
    DESCHED_POLICY_LIFETIME    = 2,  /* pod running too long */
};

/* ── A single descheduling policy ───────────────────────────────────── */

struct desched_policy {
    char     in_use;
    char     name[DESCHED_POLICY_NAME_MAX];
    enum desched_policy_type type;
    uint64_t threshold;         /* e.g. 30 for 30% */
    uint64_t lifetime_ms;       /* for LIFETIME policy */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * cluster_descheduler_init() — Initialise the descheduler subsystem.
 * Returns 0 on success.
 */
int cluster_descheduler_init(void);

/**
 * cluster_descheduler_run() — Evaluate all registered policies and
 * perform evictions as needed.  Returns the number of pods evicted.
 */
int cluster_descheduler_run(void);

/**
 * cluster_descheduler_add_policy() — Register a new descheduling policy.
 * @name:      Human-readable policy name.
 * @type:      Policy type (LOW_UTIL, HIGH_UTIL, LIFETIME).
 * @threshold: Threshold value (utilisation percentage, etc.).
 * Returns 0 on success, negative on error.
 */
int cluster_descheduler_add_policy(const char *name,
                                   enum desched_policy_type type,
                                   uint64_t threshold);

/**
 * cluster_descheduler_remove_policy() — Unregister a policy by name.
 */
int cluster_descheduler_remove_policy(const char *name);

/**
 * cluster_descheduler_get_stats() — Query eviction statistics.
 */
int cluster_descheduler_get_stats(uint64_t *total_evictions);

#endif /* CLUSTER_DESCHEDULER_H */
