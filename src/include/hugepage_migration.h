#ifndef HUGEPAGE_MIGRATION_H
#define HUGEPAGE_MIGRATION_H

#include "types.h"

/*
 * Huge page migration — migrate transparent huge pages (2 MB) between
 * NUMA nodes or for memory compaction.
 *
 * If a THP migration fails (e.g., insufficient contiguous memory at the
 * target), the huge page is split into 4K base pages, which are then
 * migrated individually.
 */

/* ── Huge page migration API ───────────────────────────────────────── */

/*
 * Migrate a transparent huge page (2 MB) to the target NUMA node.
 * Copies the entire 2 MB page to a newly allocated 2 MB block on the
 * target node, then updates page table entries.
 *
 * @phys_addr:  Physical address of the huge page (2 MB aligned)
 * @target_node: Destination NUMA node ID
 *
 * Returns 0 on success, -errno on failure.
 */
int migrate_huge_page(uint64_t phys_addr, int target_node);

/*
 * Split a huge page into 4K base pages when migration of the huge page
 * itself fails or is undesirable.  After splitting, each 4K page can be
 * migrated independently.
 *
 * @phys_addr:  Physical address of the huge page
 *
 * Returns the number of 4K pages created, or negative errno.
 */
int split_huge_page_for_migration(uint64_t phys_addr);

/*
 * Check whether the given physical address is a tracked huge page
 * that can be migrated.
 */
int is_migratable_huge_page(uint64_t phys_addr);

/* ── Tracking counters (NR_ISOLATED_ANON + NR_ISOLATED_FILE) ───────── */

/* Increment isolated page count when isolating THP pages for migration */
void hugepage_inc_isolated(int anon, int nr_pages);

/* Decrement isolated page count after migration completes */
void hugepage_dec_isolated(int anon, int nr_pages);

/* Get the current isolated page count */
uint64_t hugepage_get_isolated_anon(void);
uint64_t hugepage_get_isolated_file(void);

/* ── Initialisation ────────────────────────────────────────────────── */

void hugepage_migration_init(void);

#endif /* HUGEPAGE_MIGRATION_H */
