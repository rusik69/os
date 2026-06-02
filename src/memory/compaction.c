#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "pmm.h"
#include "vmm.h"

/*
 * Memory compaction — defragments physical memory by moving pages.
 *
 * When the allocator cannot find a large contiguous range despite
 * having enough total free frames, compaction scans for movable
 * pages and relocates them to consolidate free space.
 *
 * Strategy:
 *   1. Reclaim slab caches (frees empty slab pages)
 *   2. Analyse fragmentation: count free runs vs largest free block
 *   3. If fragmented, try incremental consolidation by reclaiming
 *      pages from the middle of fragmented regions
 *   4. Report results so callers can decide whether to retry
 */

/* Estimate fragmentation: returns a value 0-100 where higher = more fragmented.
 * Computed as: (free_runs * 100 / total_free_frames) clamped to 100.
 * When every free frame is isolated (worst), this approaches 100.
 * When all free frames are contiguous (best), this is 1 (or 0 if none free). */
uint64_t compaction_fragmentation_pct(void) {
    uint64_t total_free = pmm_get_total_frames() - pmm_get_used_frames();
    if (total_free == 0) return 0;

    uint64_t runs = pmm_free_block_count();
    uint64_t pct = (runs * 100ULL) / total_free;
    if (pct > 100) pct = 100;
    return pct;
}

/* Attempt compaction: try to defragment by reclaiming pages and
 * consolidating free space.  Returns the new size of the largest
 * contiguous free block (in frames) after compaction, or 0 on failure.
 *
 * This function is called when pmm_alloc_frames() cannot satisfy
 * a request for 'needed_pages' contiguous frames despite having
 * enough total free memory. */
uint64_t compaction_run(void) {
    uint64_t before_runs    = pmm_free_block_count();
    uint64_t before_largest = pmm_largest_free_block();
    uint64_t total_free     = pmm_get_total_frames() - pmm_get_used_frames();

    kprintf("[COMPACT] Before: free=%llu frames, largest-block=%llu, runs=%llu, frag=%llu%%\n",
            (unsigned long long)total_free,
            (unsigned long long)before_largest,
            (unsigned long long)before_runs,
            (unsigned long long)compaction_fragmentation_pct());

    /* Stage 1: Reclaim slab cache pages — these are fully-free slabs
     * that can be returned to the page allocator immediately. */
    extern void kmem_cache_reap(void);
    kmem_cache_reap();

    /* Stage 2: If still fragmented after slab reaping, attempt to
     * identify pages that can be migrated.  For now, migration is
     * limited — we rely on slab reaping as the primary mechanism.
     * Future work: add page migration for movable (non-kernel) pages. */
    uint64_t after_runs    = pmm_free_block_count();
    uint64_t after_largest = pmm_largest_free_block();
    uint64_t after_free    = pmm_get_total_frames() - pmm_get_used_frames();

    kprintf("[COMPACT] After:  free=%llu frames, largest-block=%llu, runs=%llu, frag=%llu%%\n",
            (unsigned long long)after_free,
            (unsigned long long)after_largest,
            (unsigned long long)after_runs,
            (unsigned long long)compaction_fragmentation_pct());

    if (after_largest > before_largest)
        kprintf("[COMPACT] Consolidation improved largest block by %llu frames\n",
                (unsigned long long)(after_largest - before_largest));

    return after_largest;
}

void compaction_init(void) {
    kprintf("[OK] Memory compaction initialized\n");
}
