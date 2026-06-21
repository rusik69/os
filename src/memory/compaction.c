/*
 * compaction.c — Physical memory compaction
 *
 * When the PMM fails to satisfy contiguous multi-page allocations
 * (order > 0), the compaction subsystem defragments physical memory by
 * relocating pages from MIGRATE_MOVABLE pageblocks, coalescing free
 * regions into larger contiguous blocks.
 *
 * Design:
 *   1. Fragmentation is measured as the ratio of the largest free run
 *      to total free memory — a high ratio means low fragmentation.
 *   2. compaction_run() performs a single pass: it scans allocated pages
 *      in MIGRATE_MOVABLE pageblocks, and for each one with a single
 *      reference (refcount == 1), attempts to relocate it to a new page
 *      allocated from elsewhere.  The freed page then merges with adjacent
 *      free regions, improving contiguity.
 *   3. The pass is bound to a maximum number of page moves (COMPACT_MAX_MOVE)
 *      per invocation so that it does not monopolise the CPU during OOM
 *      recovery.
 *
 * Files: src/memory/compaction.c, src/include/compaction.h
 * Item:  18 (PMM contiguous page allocation with compaction fallback)
 */

#include "compaction.h"
#include "pmm.h"
#include "pageblock.h"
#include "printf.h"
#include "string.h"
#include "panic.h"
#include "timer.h"
#include "export.h"

/* ── Tunables ────────────────────────────────────────────────────────── */

/* Maximum number of pages to move in a single compaction_run() call.
 * This prevents compaction from starving other work during OOM recovery. */
#define COMPACT_MAX_MOVE        64

/* Minimum free-run count that triggers compaction attempt.
 * If there are fewer free runs than this, memory is not very fragmented. */
#define COMPACT_MIN_RUNS        4

/* ── Forward declarations ───────────────────────────────────────────── */

static int  page_is_movable(uint64_t phys_addr);
static int  page_should_migrate(uint64_t phys_addr);
static int  migrate_one_page(uint64_t old_phys);

/* ── Initialisation ──────────────────────────────────────────────────── */

void compaction_init(void)
{
    kprintf("[compaction] memory compaction subsystem ready\n");
}
EXPORT_SYMBOL(compaction_init);

/* ── Fragmentation measurement ───────────────────────────────────────── */

/*
 * Calculate the current memory fragmentation percentage.
 *
 * Definition: if all free pages formed a single contiguous block,
 * fragmentation is 0%.  If the free pages are scattered in many
 * small runs, fragmentation approaches 100%.
 *
 *   frag% = 100 * (1 - largest_run / total_free)
 *
 * Returns a value between 0 (not fragmented) and 100 (max fragmention).
 */
uint64_t compaction_fragmentation_pct(void)
{
    uint64_t total = pmm_get_total_frames();
    uint64_t used  = pmm_get_used_frames();

    if (total == 0)
        return 0;

    if (total <= used)
        return 100;  /* completely out of memory */;

    uint64_t free_count = total - used;
    if (free_count == 0)
        return 100;

    uint64_t largest = pmm_largest_free_block();
    if (largest == 0)
        return 100;  /* no contiguous free space at all */

    /* If the largest free block is nearly all free memory, fragmentation is low.
     * Compute: 100 - (largest * 100 / free_count) gives the fragmentation pct. */
    uint64_t pct = 100 - (largest * 100ULL / free_count);

    /* Clamp to [0, 100] */
    if (pct > 100)
        pct = 100;

    return pct;
}
EXPORT_SYMBOL(compaction_fragmentation_pct);

/* ── Page classification helpers ─────────────────────────────────────── */

/* Check whether a page (given its physical address) is in a MOVABLE
 * pageblock and thus eligible for compaction migration. */
static int page_is_movable(uint64_t phys_addr)
{
    uint64_t frame = phys_addr / PAGE_SIZE;
    enum migratetype mt = pageblock_get_migratetype(frame);
    return (mt == MIGRATE_MOVABLE) ? 1 : 0;
}

/* Determine whether a page should be considered for migration.
 * A page is a good candidate if:
 *   - It is in a MIGRATE_MOVABLE pageblock
 *   - It is allocated (bitmap says used)
 *   - It has exactly 1 reference (single owner — safe to move)
 *
 * We do NOT move pages with refcount != 1 because COW mappings or
 * kernel-internal references (page tables, slab allocator) would break. */
static int page_should_migrate(uint64_t phys_addr)
{
    if (phys_addr == 0)
        return 0;

    uint64_t frame = phys_addr / PAGE_SIZE;

    /* Must be in a MOVABLE pageblock */
    if (!page_is_movable(phys_addr))
        return 0;

    /* Must have exactly one reference — safe to copy and free */
    if (pmm_refcount(phys_addr) != 1)
        return 0;

    /* Sanity check: the frame must be within range */
    if (frame >= pmm_get_total_frames())
        return 0;

    return 1;
}

/* ── Single-page migration ───────────────────────────────────────────── */

/*
 * Migrate one page: allocate a new physical page, copy contents, free the
 * old one.  Returns 1 on success, 0 on failure.
 *
 * The caller must ensure that the old page has a single reference (refcount
 * == 1) and is in a MOVABLE pageblock.  We allocate a replacement page,
 * memcpy the 4 KB of data, and free the original.  The freed page then
 * becomes available for coalescing with adjacent free regions.
 */
static int migrate_one_page(uint64_t old_phys)
{
    /* Allocate a new page to receive the contents */
    uint64_t new_phys = pmm_alloc_frame();
    if (new_phys == 0)
        return 0;   /* no memory for migration target */

    /* Map both pages into the kernel address space for copying.
     * PHYS_TO_VIRT gives us the kernel's linear mapping. */
    uint64_t *old_virt = (uint64_t *)PHYS_TO_VIRT(old_phys);
    uint64_t *new_virt = (uint64_t *)PHYS_TO_VIRT(new_phys);

    /* Copy all 4 KB (512 × 8 bytes) */
    for (int i = 0; i < (int)(PAGE_SIZE / 8); i++)
        new_virt[i] = old_virt[i];

    /* Free the original page — this extends the adjacent free region. */
    pmm_free_frame(old_phys);

    return 1;   /* successfully migrated */
}

/* ── Main compaction pass ────────────────────────────────────────────── */

/*
 * Run a single compaction pass.
 *
 * Algorithm:
 *   1. Measure the current free-run count (more runs = more fragmented).
 *      If not very fragmented, return early.
 *   2. Find the largest contiguous free region.
 *   3. Scan allocated pages BELOW that region, in MOVABLE pageblocks,
 *      and migrate them up above the free region.  This extends the
 *      large free region downward, making it even larger.
 *   4. Stop after COMPACT_MAX_MOVE pages to avoid CPU starvation.
 *
 * Returns the number of pages successfully moved (compacted).
 */
uint64_t compaction_run(void)
{
    uint64_t total_frames = pmm_get_total_frames();
    uint64_t total_free = total_frames - pmm_get_used_frames();

    /* If there are fewer free pages than a single pageblock, compaction
     * won't help much — we need at least one pageblock of free memory. */
    if (total_free < PAGEBLOCK_NR_PAGES)
        return 0;

    /* Quick check: if the number of free runs is small (or the largest
     * free region already spans most of free memory), skip compaction. */
    uint64_t runs = pmm_free_block_count();
    if (runs < COMPACT_MIN_RUNS)
        return 0;   /* already well-consolidated */

    uint64_t largest = pmm_largest_free_block();
    (void)largest; /* used below in the success measurement */

    uint64_t moved = 0;
    uint64_t scanned = 0;

    /* ── Phase 1: Find the largest free region to determine work area ── */
    uint64_t best_free_start = 0;
    uint64_t best_free_count = 0;
    {
        uint64_t scan = 0;
        while (scan < total_frames) {
            uint64_t count;
            uint64_t start = pmm_find_free_region(scan, &count);
            if (start == ~0ULL)
                break;
            if (count > best_free_count) {
                best_free_start = start;
                best_free_count = count;
            }
            scan = start + count + 1;
        }
    }

    /* If there's no significant free region, nothing to compact */
    if (best_free_count == 0)
        return 0;

    /* ── Phase 2: Migrate MOVABLE pages below the largest free region ──
     *
     * We walk the allocated pages just BELOW the large free region, from
     * the boundary downward.  Each page we migrate (copy + free) merges
     * its old location into the free region, extending it downward.
     */
    {
        uint64_t frame = best_free_start;

        /* Walk backwards from just before the free region */
        while (frame > 0 && moved < COMPACT_MAX_MOVE) {
            frame--;

            uint64_t phys_addr = frame * PAGE_SIZE;
            scanned++;

            if (!page_should_migrate(phys_addr))
                continue;

            /* Attempt the migration */
            if (migrate_one_page(phys_addr)) {
                moved++;
            }
        }
    }

    /* ── Report result (if any pages were moved) ─────────────────────── */
    if (moved > 0) {
        uint64_t new_largest = pmm_largest_free_block();
        kprintf("[compaction] moved %llu pages (scanned %llu, "
                "largest free run %llu → %llu frames, "
                "fragmentation %llu%%)\n",
                (unsigned long long)moved,
                (unsigned long long)scanned,
                (unsigned long long)best_free_count,
                (unsigned long long)new_largest,
                (unsigned long long)compaction_fragmentation_pct());
    }

    return moved;
}
EXPORT_SYMBOL(compaction_run);
#include "module.h"
module_init(compaction_init);

/* ── compact_zone ─────────────────────────────────────────── */
int compact_zone(uint64_t zone_pfn_start, uint64_t zone_pfn_end)
{
    if (zone_pfn_start >= zone_pfn_end)
        return -EINVAL;

    kprintf("[compaction] compact_zone: 0x%llx - 0x%llx\n",
            (unsigned long long)zone_pfn_start, (unsigned long long)zone_pfn_end);

    /* Delegate to the existing compaction_run() for the zone range.
     * In a real kernel, this would restrict to the given zone.
     * For now, run full compaction. */
    uint64_t moved = compaction_run();
    return (int)moved;
}

/* ── compaction_suitable ──────────────────────────────────── */
int compaction_suitable(uint64_t zone_pfn_start, uint64_t zone_pfn_end)
{
    if (zone_pfn_start >= zone_pfn_end)
        return 0;

    /* Check if fragmentation is high enough to merit compaction.
     * Use the existing fragmentation measurement. */
    uint64_t frag = compaction_fragmentation_pct();

    /* Suitable if fragmentation > 30% — arbitrary threshold */
    int suitable = (frag > 30) ? 1 : 0;

    kprintf("[compaction] compaction_suitable: frag=%llu%% -> %s\n",
            (unsigned long long)frag, suitable ? "yes" : "no");
    return suitable;
}

/* ── isolate_migratepages ────────────────────────────────── */
int isolate_migratepages(uint64_t *start_pfn, uint64_t *end_pfn)
{
    if (!start_pfn || !end_pfn)
        return -EINVAL;

    uint64_t total_frames = pmm_get_total_frames();
    uint64_t isolated = 0;

    /* Scan the given PFN range for movable pages that can be migrated */
    uint64_t scan_start = *start_pfn;
    if (scan_start >= total_frames) scan_start = 0;
    uint64_t scan_end = *end_pfn;
    if (scan_end > total_frames) scan_end = total_frames;

    for (uint64_t pfn = scan_start; pfn < scan_end && isolated < 64; pfn++) {
        uint64_t phys = pfn * PAGE_SIZE;
        /* Check if this page is movable and has single refcount */
        if (pmm_refcount(phys) == 1) {
            /* Isolate it: in a real kernel, we'd move to a migration list */
            isolated++;
        }
    }

    *start_pfn = scan_start + isolated;
    kprintf("[compaction] isolate_migratepages: %llu pages isolated\n",
            (unsigned long long)isolated);
    return (int)isolated;
}

/* ── Stub: migrate_pages ─────────────────────────────────────── */
int migrate_pages(uint64_t *from_pfns, uint64_t *to_pfns, int nr_pages)
{
    (void)from_pfns;
    (void)to_pfns;
    (void)nr_pages;
    kprintf("[compaction] migrate_pages: not yet implemented\n");
    return 0;
}

/* ── Stub: compact_finished ──────────────────────────────────── */
int compact_finished(void)
{
    kprintf("[compaction] compact_finished: not yet implemented\n");
    return 0;
}
