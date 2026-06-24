#ifndef PAGE_POOL_H
#define PAGE_POOL_H

#include "types.h"
#include "spinlock.h"

/* ── Page Pool constants ────────────────────────────────────────────── */

#define PAGE_POOL_MAX_POOLS     8   /* maximum number of static pools */
#define PAGE_POOL_INIT_SIZE     256 /* default pages per pool */

/* Flags for page_pool_init */
#define PAGE_POOL_F_DMA         0x01 /* pages suitable for DMA (low 16MB) */
#define PAGE_POOL_F_NONEXEC     0x02 /* pages with NX bit set */

/* ── Page pool structure ────────────────────────────────────────────── */

struct page_pool {
    int      initialized;
    uint32_t flags;
    uint32_t order;            /* allocation order (0 = 4KB, 1 = 8KB, etc.) */
    uint32_t size;             /* total capacity (number of pages) */
    uint32_t count;            /* currently available pages in pool */
    uint64_t pages[PAGE_POOL_INIT_SIZE]; /* array of physical addresses */
    spinlock_t lock;
    /* Stats */
    uint64_t alloc_count;
    uint64_t return_count;
    uint64_t refill_count;
    uint64_t oom_count;
} __cacheline_aligned;

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialise the page pool subsystem.  Must be called once during boot. */
void page_pool_subsys_init(void);

/* Initialise a single page pool.
 * @pool_idx: Pool index (0 .. PAGE_POOL_MAX_POOLS-1)
 * @flags:    PAGE_POOL_F_* flags
 * @order:    Allocation order (0 = single page)
 * @size:     Initial number of pages in the pool (max PAGE_POOL_INIT_SIZE)
 * Returns 0 on success, negative errno on failure.
 */
int page_pool_init(int pool_idx, uint32_t flags, uint32_t order, uint32_t size);

/* Allocate a page from the pool.
 * Returns physical address of the page, or 0 on failure.
 */
uint64_t page_pool_alloc_page(int pool_idx);

/* Return a page to the pool.  If the pool is above a refill threshold,
 * the page is returned to the buddy allocator instead.
 * @pool_idx: Pool index
 * @page:     Physical address of the page to return
 */
void page_pool_return_page(int pool_idx, uint64_t page);

/* Destroy a pool and free all its pages back to the buddy allocator. */
void page_pool_destroy(int pool_idx);

/* Get a specific pool by index.  Returns NULL if not initialised. */
struct page_pool *page_pool_get(int pool_idx);

/* Driver hook: e1000 RX allocation using page pool (pool 0 by default).
 * Returns a physical address suitable for DMA. */
uint64_t page_pool_alloc_rx_page(void);

/* Return a RX page to the pool. */
void page_pool_return_rx_page(uint64_t page);

#endif /* PAGE_POOL_H */
