#ifndef NUMA_BALANCING_H
#define NUMA_BALANCING_H

#include "types.h"
#include "process.h"

/* ── NUMA balancing constants ──────────────────────────────────────── */

#define NUMA_MAX_NODES      16
#define NUMA_SCAN_PERIOD_MS 1000   /* 1 second between scan cycles */
#define NUMA_SCAN_PAGES_PER_CYCLE 256  /* pages to scan per cycle */
#define NUMA_HINT_MAX       64     /* max tracked hint faults per node */

/* ── Per-node page access tracking ─────────────────────────────────── */

struct numa_node_stats {
    uint64_t local_allocations;   /* pages allocated on this node */
    uint64_t remote_accesses;     /* hints from remote node accesses */
    uint64_t migrations_in;       /* pages migrated to this node */
    uint64_t migrations_out;      /* pages migrated away from this node */
    uint64_t pages_placed;        /* pages placed on this node */
};

/* ── Public API ────────────────────────────────────────────────────── */

/* Initialise the NUMA balancing subsystem. */
void numa_balancing_init(void);

/* Called from the page fault handler when a remote NUMA access is
 * detected (PTE accessed/dirty bits indicate access, and the faulting
 * node differs from the page's home node).
 * @addr:  Faulting virtual address
 * @node:  NUMA node of the accessing CPU
 */
void numa_hint_fault(uint64_t addr, int node);

/* Migrate a page to the target NUMA node using the PMM migration path.
 * @page:   Physical address of the page (frame number << 12)
 * @target_node: Destination NUMA node ID
 * Returns 0 on success, negative errno on failure.
 */
int numa_migrate_page(uint64_t page, int target_node);

/* Get per-node statistics.
 * @node:  NUMA node ID
 * Returns pointer to node stats, or NULL if node is invalid.
 */
const struct numa_node_stats *numa_get_node_stats(int node);

/* Enable/disable the periodic NUMA scanner. */
void numa_scan_set_enabled(int enabled);
int  numa_scan_is_enabled(void);

/* Debugfs read callback for /sys/kernel/debug/numa_stats */
void numa_stats_read(char *buf, int *len);

#endif /* NUMA_BALANCING_H */
