/*
 * hugetlb.h — HugeTLB page pool for explicit huge page allocation
 *
 * Provides a pre-allocated pool of 2MB huge pages that users can
 * request via mmap(MAP_HUGETLB).  This guarantees the use of 2MB
 * page-table entries and returns an error if the pool is exhausted
 * (rather than silently falling back to 4KB pages).
 *
 * Item 128: HugeTLB — explicit 2M/1G page pool
 */

#ifndef HUGETLB_H
#define HUGETLB_H

#include "types.h"

/* ── Configuration ───────────────────────────────────────────────────────
 * Default pool size: 64 huge pages (128 MB total).
 * Adjustable at boot.  1G pages are not yet supported.
 */
#define HUGETLB_DEFAULT_POOL_SIZE   64      /* number of 2MB huge pages */
#define HUGETLB_MAX_POOL_SIZE       1024    /* safety cap (2 GB) */

/* Huge page size constants */
#define HUGETLB_PAGE_SHIFT      21          /* 2MB = 2^21 */
#define HUGETLB_PAGE_SIZE       (1ULL << HUGETLB_PAGE_SHIFT)
#define HUGETLB_PAGE_NFRAMES    (HUGETLB_PAGE_SIZE / 4096)  /* 512 */

/* mmap flag — must not conflict with existing MAP_* values */
#define MAP_HUGETLB             0x40000     /* Linux-compatible value */
#define MAP_HUGE_2MB            0x4000000   /* encode 2MB shift (21) << MAP_HUGE_SHIFT */

/* ── Public API ───────────────────────────────────────────────────────── */

/* Initialise the HugeTLB pool: pre-allocate @count 2MB huge pages.
 * Called once during kernel boot.  May be called again to resize.
 * Returns 0 on success, -ENOMEM if insufficient contiguous memory. */
int hugetlb_init(uint32_t count);

/* Allocate one 2MB huge page from the pool.
 * Returns the physical address (2MB-aligned), or 0 if pool exhausted. */
uint64_t hugetlb_alloc_frame(void);

/* Return a 2MB huge page to the pool.  @phys must be 2MB-aligned and
 * previously returned by hugetlb_alloc_frame(). */
void hugetlb_free_frame(uint64_t phys);

/* Return the number of free 2MB huge pages currently in the pool. */
uint32_t hugetlb_available(void);

/* Return the total pool capacity. */
uint32_t hugetlb_pool_size(void);

#endif /* HUGETLB_H */
