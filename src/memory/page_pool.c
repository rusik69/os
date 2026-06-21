/* page_pool.c — DMA page recycling pool for network drivers.
 *
 * Provides a static set of page pools (max 8) that network drivers
 * can use to recycle DMA pages instead of allocating/freeing from the
 * buddy allocator on every RX/TX cycle.  Each pool holds up to 256
 * pages (PAGE_POOL_INIT_SIZE).
 *
 * Driver hook: e1000_rx_alloc() uses pool 0 by default.
 */

#define KERNEL_INTERNAL
#include "page_pool.h"
#include "pmm.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "errno.h"

/* ── Global state ──────────────────────────────────────────────────── */

static struct page_pool g_page_pools[PAGE_POOL_MAX_POOLS];
static int g_page_pool_subsys_inited = 0;

/* Default pool index for e1000 RX */
#define PAGE_POOL_RX_IDX 0

/* Refill threshold: if count drops below this, next return_page will
 * free the page to buddy instead of recycling (to help the pool recover
 * memory pressure). */
#define PAGE_POOL_REFILL_THRESH (PAGE_POOL_INIT_SIZE / 4)

/* ── Subsystem initialisation ──────────────────────────────────────── */

void page_pool_subsys_init(void)
{
    if (g_page_pool_subsys_inited) return;

    memset(g_page_pools, 0, sizeof(g_page_pools));
    g_page_pool_subsys_inited = 1;

    kprintf("[page_pool] subsystem initialised (%d pools max, %d pages/pool)\n",
            PAGE_POOL_MAX_POOLS, PAGE_POOL_INIT_SIZE);
}

/* ── Pool initialisation ────────────────────────────────────────────── */

int page_pool_init(int pool_idx, uint32_t flags, uint32_t order, uint32_t size)
{
    if (!g_page_pool_subsys_inited)
        return -EAGAIN;
    if (pool_idx < 0 || pool_idx >= PAGE_POOL_MAX_POOLS)
        return -EINVAL;
    if (size > PAGE_POOL_INIT_SIZE)
        size = PAGE_POOL_INIT_SIZE;
    if (size == 0)
        size = PAGE_POOL_INIT_SIZE;

    if (g_page_pools[pool_idx].initialized)
        return -EEXIST;

    struct page_pool *pool = &g_page_pools[pool_idx];
    memset(pool, 0, sizeof(*pool));

    pool->flags = flags;
    pool->order = order;
    pool->size  = size;
    pool->count = 0;
    spinlock_init(&pool->lock);

    /* Pre-allocate the initial page pool */
    int actual_order = (int)order;
    for (uint32_t i = 0; i < size; i++) {
        uint64_t page = pmm_alloc_frame();
        if (!page) {
            /* Free any pages already allocated */
            for (uint32_t j = 0; j < i; j++) {
                pmm_free_frame(pool->pages[j]);
            }
            pool->count = 0;
            kprintf("[page_pool] pool %d: OOM during init (allocated %u of %u)\n",
                    pool_idx, i, size);
            pool->oom_count = size - i;
            return -ENOMEM;
        }
        pool->pages[i] = page;
        pool->count++;
    }

    pool->initialized = 1;

    kprintf("[page_pool] pool %d initialised: flags=0x%x order=%u size=%u\n",
            pool_idx, (unsigned int)flags, order, size);

    return 0;
}

/* ── Allocation ─────────────────────────────────────────────────────── */

uint64_t page_pool_alloc_page(int pool_idx)
{
    if (!g_page_pool_subsys_inited) return 0;
    if (pool_idx < 0 || pool_idx >= PAGE_POOL_MAX_POOLS) return 0;

    struct page_pool *pool = &g_page_pools[pool_idx];
    if (!pool->initialized) return 0;

    uint64_t flags;
    spinlock_irqsave_acquire(&pool->lock, &flags);

    uint64_t page = 0;

    if (pool->count > 0) {
        pool->count--;
        page = pool->pages[pool->count];
        pool->pages[pool->count] = 0;
        pool->alloc_count++;
    }

    spinlock_irqsave_release(&pool->lock, flags);

    if (!page) {
        /* Pool empty — allocate from buddy as fallback */
        page = pmm_alloc_frame();
        if (page) {
            pool->oom_count++;
        }
    }

    return page;
}

/* ── Return / recycle ────────────────────────────────────────────────── */

void page_pool_return_page(int pool_idx, uint64_t page)
{
    if (!g_page_pool_subsys_inited || page == 0) return;
    if (pool_idx < 0 || pool_idx >= PAGE_POOL_MAX_POOLS) {
        pmm_free_frame(page);
        return;
    }

    struct page_pool *pool = &g_page_pools[pool_idx];
    if (!pool->initialized) {
        pmm_free_frame(page);
        return;
    }

    uint64_t flags;
    spinlock_irqsave_acquire(&pool->lock, &flags);

    if (pool->count < pool->size) {
        /* Recycle the page back into the pool */
        pool->pages[pool->count] = page;
        pool->count++;
        pool->return_count++;
    } else {
        /* Pool is full — refill threshold: free to buddy */
        pmm_free_frame(page);
        pool->refill_count++;
    }

    spinlock_irqsave_release(&pool->lock, flags);
}

/* ── Destroy ─────────────────────────────────────────────────────────── */

void page_pool_destroy(int pool_idx)
{
    if (pool_idx < 0 || pool_idx >= PAGE_POOL_MAX_POOLS) return;

    struct page_pool *pool = &g_page_pools[pool_idx];
    if (!pool->initialized) return;

    uint64_t flags;
    spinlock_irqsave_acquire(&pool->lock, &flags);

    /* Free all pages in the pool */
    for (uint32_t i = 0; i < pool->count; i++) {
        if (pool->pages[i]) {
            pmm_free_frame(pool->pages[i]);
            pool->pages[i] = 0;
        }
    }

    pool->count = 0;
    pool->initialized = 0;

    spinlock_irqsave_release(&pool->lock, flags);

    kprintf("[page_pool] pool %d destroyed (alloc_count=%llu return_count=%llu "
            "refill_count=%llu oom_count=%llu)\n",
            pool_idx,
            (unsigned long long)pool->alloc_count,
            (unsigned long long)pool->return_count,
            (unsigned long long)pool->refill_count,
            (unsigned long long)pool->oom_count);
}

/* ── Pool accessor ───────────────────────────────────────────────────── */

struct page_pool *page_pool_get(int pool_idx)
{
    if (pool_idx < 0 || pool_idx >= PAGE_POOL_MAX_POOLS) return NULL;
    if (!g_page_pools[pool_idx].initialized) return NULL;
    return &g_page_pools[pool_idx];
}

/* ── Driver hook: e1000 RX allocation ─────────────────────────────────
 *
 * These functions provide a convenient API for the e1000 driver to
 * allocate/recycle RX pages from pool 0.  The e1000 driver can call
 * page_pool_alloc_rx_page() instead of direct pmm_alloc_frame().
 */

uint64_t page_pool_alloc_rx_page(void)
{
    return page_pool_alloc_page(PAGE_POOL_RX_IDX);
}

void page_pool_return_rx_page(uint64_t page)
{
    page_pool_return_page(PAGE_POOL_RX_IDX, page);
}
#include "module.h"
module_init(page_pool_subsys_init);

/* ── Stub: page_pool_alloc ─────────────────────────────── */
void* page_pool_alloc(int flags)
{
    (void)flags;
    kprintf("[page_pool] page_pool_alloc: not yet implemented\n");
    return 0;
}
/* ── Stub: page_pool_free ─────────────────────────────── */
int page_pool_free(void *page)
{
    (void)page;
    kprintf("[page_pool] page_pool_free: not yet implemented\n");
    return 0;
}
/* ── Stub: page_pool_refill ─────────────────────────────── */
int page_pool_refill(int count)
{
    (void)count;
    kprintf("[page_pool] page_pool_refill: not yet implemented\n");
    return 0;
}
