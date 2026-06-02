#ifndef KSM_H
#define KSM_H

#include "types.h"

/* KSM — Kernel Same-page Merging with Scan Pacing and NUMA Awareness
 *
 * Features:
 *   - Incremental scanning with position tracking
 *   - Adaptive batch size based on memory pressure (free page ratio)
 *   - NUMA-aware merging (prefer same-node pages)
 *   - Pause scanning under severe memory pressure
 */

/* Initialize KSM subsystem */
void ksm_init(void);

/* Enable/disable KSM scanning */
void ksm_set_enabled(int enabled);
int  ksm_is_enabled(void);

/* Register a memory region for KSM scanning.
 * @addr:      Virtual address (must be page-aligned)
 * @size:      Size in bytes (must be page-aligned)
 * @numa_node: NUMA node ID of the pages (0 for single-node systems)
 * Returns 0 on success, -1 on failure (table full or misaligned).
 */
int ksm_register_region(uint64_t addr, size_t size, int numa_node);

/* Legacy wrapper — registers with NUMA node 0 */
int ksm_register_region_legacy(uint64_t addr, size_t size);

/* Unregister a KSM region by virtual address */
int ksm_unregister_region(uint64_t addr);

/* Trigger a scan cycle — scans a batch of pages (size determined
 * dynamically by memory pressure).  Call from a periodic timer or
 * a background kernel thread. */
void ksm_scan_cycle(void);

/* ── Statistics ──────────────────────────────────────────────────── */

/* Number of pages successfully merged */
uint64_t ksm_get_merged_pages(void);

/* Number of same-hash pages that turned out to be different */
uint64_t ksm_get_unmergeable_pages(void);

/* Number of scan cycles performed */
uint64_t ksm_get_scan_count(void);

/* Total number of pages examined across all scan cycles */
uint64_t ksm_get_total_scanned(void);

/* Number of pages currently registered for scanning */
int ksm_get_page_count(void);

/* Current adaptive batch size (for diagnostics) */
int ksm_get_scan_batch(void);

#endif /* KSM_H */
