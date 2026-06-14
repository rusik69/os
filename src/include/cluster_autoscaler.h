#ifndef CLUSTER_AUTOSCALER_H
#define CLUSTER_AUTOSCALER_H

#include "types.h"

/*
 * Cluster autoscaler — dynamically add/remove nodes based on workload.
 *
 * Monitors pending pod count against node capacity and triggers
 * scale-up events when unschedulable pods exceed configurable thresholds.
 * A cooldown period prevents thrashing between scale events.
 */

/* ── Constants ──────────────────────────────────────────────────────── */

#define AUTOSCALER_MAX_NODES      256
#define AUTOSCALER_DEFAULT_COOLDOWN_MS (5 * 60 * 1000)  /* 5 minutes */
#define AUTOSCALER_DEFAULT_THRESHOLD  1                  /* pending pods */

/* ── Cluster autoscaler state ───────────────────────────────────────── */

struct cluster_autoscaler {
    int      enabled;
    uint32_t min_nodes;
    uint32_t max_nodes;
    uint32_t current_nodes;
    uint32_t pending_pods;
    uint64_t cpu_capacity;     /* total CPU millicores across nodes */
    uint64_t memory_capacity;  /* total memory bytes across nodes */
    uint64_t last_scale_up;    /* timestamp of last scale-up (ms) */
    uint64_t last_scale_down;  /* timestamp of last scale-down (ms) */
    uint64_t cooldown_ms;      /* min ms between scale events */
    int      threshold;        /* unschedulable pods to trigger scale-up */
};

/* ── Public API ─────────────────────────────────────────────────────── */

/**
 * cluster_autoscaler_init() — Initialise the autoscaler subsystem.
 * @min_nodes: Minimum number of nodes to keep.
 * @max_nodes: Maximum number of nodes to scale up to.
 * Returns 0 on success, negative errno on failure.
 */
int cluster_autoscaler_init(uint32_t min_nodes, uint32_t max_nodes);

/**
 * cluster_autoscaler_should_scale() — Check if scaling is needed.
 * Returns:
 *   1  — scale-up needed (pending pods > threshold, cooldown elapsed)
 *  -1  — scale-down possible (no pending pods, above min, cooldown elapsed)
 *   0  — no action required
 */
int cluster_autoscaler_should_scale(void);

/**
 * cluster_autoscaler_scale_up() — Trigger a scale-up event.
 * Increases node count by the calculated amount.
 * Returns the number of nodes added, or negative on error.
 */
int cluster_autoscaler_scale_up(void);

/**
 * cluster_autoscaler_scale_down() — Trigger a scale-down event.
 * Decreases node count by one.
 * Returns 0 on success, negative if at minimum.
 */
int cluster_autoscaler_scale_down(void);

/**
 * cluster_autoscaler_report_pending() — Report unschedulable pods.
 * @count: Number of pending/unschedulable pods.
 */
void cluster_autoscaler_report_pending(int count);

/**
 * cluster_autoscaler_update_capacity() — Update node capacity info.
 * @cpu:    Total CPU millicores.
 * @memory: Total memory bytes.
 */
void cluster_autoscaler_update_capacity(uint64_t cpu, uint64_t memory);

/**
 * cluster_autoscaler_get_info() — Get current autoscaler snapshot.
 */
void cluster_autoscaler_get_info(struct cluster_autoscaler *out);

#endif /* CLUSTER_AUTOSCALER_H */
