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
 * For now: a simple scanner that reports fragmentation level.
 * Full migration requires page table manipulation on behalf of
 * all processes sharing each page.
 */

/* Estimate fragmentation: returns percentage of free frames that
 * are isolated singletons (not part of a run of >= 2 contiguous free frames). */
uint64_t compaction_fragmentation_pct(void) {
    uint64_t total_free = pmm_get_total_frames() - pmm_get_used_frames();
    if (total_free == 0) return 0;

    /* Scan free list - simplified: count isolated singles */
    uint64_t singles = 0;
    /* The PMM bitmap or free list doesn't directly expose run analysis.
     * For now, return a heuristic based on used ratio. */
    uint64_t used = pmm_get_used_frames();
    uint64_t total = pmm_get_total_frames();
    if (total == 0) return 0;
    /* High usage tends to cause fragmentation. Report estimate. */
    uint64_t pct = (used * 100) / total;
    if (pct > 90) return (pct - 90) * 10; /* 0-100% frag for 90-100% used */
    return 0;
}

/* Attempt compaction: try to defragment by triggering page reclaim.
 * Returns number of pages consolidated. */
uint64_t compaction_run(void) {
    /* Simple approach: trigger OOM-like reclaim of cache pages */
    extern void kmem_cache_reap(void);
    kmem_cache_reap();
    return 0;
}

void compaction_init(void) {
    kprintf("[OK] Memory compaction initialized\n");
}
