#define KERNEL_INTERNAL
#include "types.h"
#include "tmpfs.h"
#include "hugetlb.h"
#include "page_allocator_ext.h"
#include "numa_mem.h"
#include "vmm.h"
#include "printf.h"
#include "errno.h"
#include "module.h"

/*
 * tmpfs_huge.c — Huge page support for tmpfs (2MB pages)
 *
 * Provides transparent 2MB page allocation for tmpfs files that are
 * large enough to benefit.  When enabled, tmpfs will attempt to back
 * file data with 2MB contiguous pages (order-9 allocations) instead
 * of 4KB pages, reducing TLB pressure and page-table overhead.
 *
 * Huge page backed inodes can be split back to 4KB pages when needed
 * (e.g., before swapping out, since the swap subsystem works with 4KB
 * page slots).
 *
 * Setting:  /sys or mount option (to be wired)
 * Default:  enabled
 */

/* ══════════════════════════════════════════════════════════════════════
 * ── Global toggle ───────────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

static int tmpfs_huge_enabled_flag = 1;

int tmpfs_huge_get_enabled(void)
{
    return tmpfs_huge_enabled_flag;
}

void tmpfs_set_huge_enabled(int enabled)
{
    tmpfs_huge_enabled_flag = enabled ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Statistics counters ─────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

static uint64_t stats_alloc_count = 0;
static uint64_t stats_free_count  = 0;
static uint64_t stats_split_count = 0;
static uint64_t stats_fail_count  = 0;

uint64_t tmpfs_huge_get_alloc_count(void) { return stats_alloc_count; }
uint64_t tmpfs_huge_get_free_count(void)  { return stats_free_count;  }
uint64_t tmpfs_huge_get_split_count(void) { return stats_split_count; }
uint64_t tmpfs_huge_get_fail_count(void)  { return stats_fail_count;  }

/* ══════════════════════════════════════════════════════════════════════
 * ── Huge page order ─────────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * The page allocator's MAX_ORDER is 10 (max 1024 pages = 4 MB per
 * allocation).  A 2 MB huge page needs 512 frames, so order 9.
 */
#define HUGEPAGE_ORDER  (HUGETLB_PAGE_SHIFT - 12)  /* 21 - 12 = 9 */

/* ══════════════════════════════════════════════════════════════════════
 * ── Allocation ──────────────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tmpfs_huge_alloc() - Allocate a 2 MB page for tmpfs.
 *
 * @node:  NUMA node to allocate from (pass numa_home_node() or -1 for
 *         default).
 *
 * Attempts an order-9 allocation (2 MB) from the general page allocator,
 * zeroed.  Returns the physical address on success, or 0 on failure.
 *
 * We intentionally do NOT use the HugeTLB pool (hugetlb_alloc_frame())
 * here because that pool is reserved for MAP_HUGETLB userspace mappings.
 * Tmpfs allocates its own huge pages from the general pool, which the
 * page allocator can coalesce from adjacent free 4 KB frames.
 */
uint64_t tmpfs_huge_alloc(int node)
{
    uint64_t phys;

    if (!tmpfs_huge_enabled_flag)
        return 0;

    if (node < 0)
        node = 0;

    phys = alloc_pages_node(node, GFP_KERNEL | GFP_ZERO, HUGEPAGE_ORDER);
    if (phys != 0) {
        stats_alloc_count++;
        return phys;
    }

    /* Allocation failed — fragmentation or insufficient contiguous
     * memory.  The caller should fall back to 4 KB page allocation. */
    stats_fail_count++;
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Deallocation ────────────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tmpfs_huge_free() - Free a 2 MB page previously allocated by
 * tmpfs_huge_alloc().
 *
 * @phys:  Physical address of the 2 MB page.  May be 0 (no-op).
 */
void tmpfs_huge_free(uint64_t phys)
{
    if (phys == 0)
        return;

    free_pages(phys, HUGEPAGE_ORDER);
    stats_free_count++;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Splitting ───────────────────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

/*
 * tmpfs_huge_split() - Mark a physical 2MB page as no longer a huge
 * page at the allocator level.
 *
 * @phys:  Physical address of the 2MB page.
 *
 * This is a no-op in the allocator layer (the page itself doesn't
 * change).  The caller (tmpfs.c) must clear the inode's is_huge flag
 * separately.
 */
void tmpfs_huge_split(uint64_t phys)
{
    (void)phys;
    stats_split_count++;
}

/* ══════════════════════════════════════════════════════════════════════
 * ── Module metadata & init ─────────────────────────────────────────
 * ══════════════════════════════════════════════════════════════════════ */

static int __init tmpfs_huge_init(void)
{
    kprintf("[OK] tmpfs huge page support initialized (2 MB pages)\n");
    return 0;
}
module_init(tmpfs_huge_init);

MODULE_LICENSE("MIT");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("tmpfs huge page support — transparent 2MB page allocation");
MODULE_AUTHOR("OS Kernel Team");
