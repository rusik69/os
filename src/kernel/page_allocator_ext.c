#include "page_allocator_ext.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"
#include "string.h"

static int page_allocator_ext_initialised = 0;

/* Track used pages via a simple bitmap (one bit per frame).
 * MAX_FRAMES covers up to the maximum physical memory we might manage. */
#define MAX_FRAMES (16ULL * 1024 * 1024) /* 64 GB of 4K pages */
static uint8_t used_bitmap[MAX_FRAMES / 8];
static uint64_t total_allocated_pages = 0;

void page_allocator_ext_init(void)
{
    if (page_allocator_ext_initialised)
        return;

    memset(used_bitmap, 0, sizeof(used_bitmap));
    total_allocated_pages = 0;
    page_allocator_ext_initialised = 1;
    kprintf("page_allocator_ext: initialised (GFP wrapper)\n");
}

static inline void bitmap_set(uint64_t idx)
{
    if (idx < MAX_FRAMES)
        used_bitmap[idx / 8] |= (1 << (idx % 8));
}

static inline void bitmap_clear(uint64_t idx)
{
    if (idx < MAX_FRAMES)
        used_bitmap[idx / 8] &= ~(1 << (idx % 8));
}

static inline int bitmap_test(uint64_t idx)
{
    if (idx >= MAX_FRAMES)
        return 0;
    return (used_bitmap[idx / 8] >> (idx % 8)) & 1;
}

uint64_t alloc_pages(int gfp_mask, int order)
{
    if (!page_allocator_ext_initialised)
        return 0;

    int atomic_ok = (gfp_mask & GFP_ATOMIC) != 0;
    int zero = (gfp_mask & GFP_ZERO) != 0;
    (void)atomic_ok;  /* reserve for future use — GFP_ATOMIC disables sleep */

    uint64_t num_pages = 1ULL << order;
    uint64_t frame = 0;
    int found = 0;

    /* Allocate from the base physical allocator.
     * For single-page allocations (order=0) we use pmm_alloc_frame().
     * For larger orders we use pmm_alloc_frames() for contiguous blocks. */
    if (order == 0) {
        frame = pmm_alloc_frame();
        if (frame == 0)
            return 0;  /* OOM */
        found = 1;
    } else {
        /* Use the PMM's contiguous frame allocator for order > 0.
         * pmm_alloc_frames returns the physical address of the first
         * page in a contiguous block of 2^order pages, or NULL. */
        uint64_t *block = pmm_alloc_frames(num_pages);
        if (block) {
            for (uint64_t i = 0; i < num_pages; i++) {
                uint64_t idx = ((uint64_t)block / PAGE_SIZE) + i;
                bitmap_set(idx);
            }
            frame = (uint64_t)block;
            found = 1;
        }
    }

    if (!found)
        return 0;

    /* Track in our bitmap. */
    {
        uint64_t idx = frame / PAGE_SIZE;
        for (uint64_t i = 0; i < num_pages; i++) {
            bitmap_set(idx + i);
        }
        total_allocated_pages += num_pages;
    }

    /* Zero if requested. */
    if (zero) {
        void *virt = PHYS_TO_VIRT(frame);
        memset(virt, 0, num_pages * PAGE_SIZE);
    }

    return frame;
}

void free_pages(uint64_t addr, int order)
{
    if (!page_allocator_ext_initialised)
        return;
    if (addr == 0)
        return;
    if ((addr & 0xFFF) != 0)
        return;

    uint64_t num_pages = 1ULL << order;

    /* Poison the pages in the kernel virtual mapping before freeing. */
    void *virt = PHYS_TO_VIRT(addr);
    memset(virt, 0xDC, num_pages * PAGE_SIZE);

    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t phys = addr + i * PAGE_SIZE;
        uint64_t idx = phys / PAGE_SIZE;
        bitmap_clear(idx);
        pmm_free_frame(phys);
    }
    total_allocated_pages -= num_pages;
}

uint64_t get_zeroed_page(int gfp_mask)
{
    return alloc_pages(gfp_mask | GFP_ZERO, 0);
}

uint64_t page_allocator_ext_get_available(void)
{
    if (!page_allocator_ext_initialised)
        return 0;
    return pmm_get_total_frames() - total_allocated_pages;
}

uint64_t page_allocator_ext_get_used(void)
{
    if (!page_allocator_ext_initialised)
        return 0;
    return total_allocated_pages;
}

/* ── Stub: page_alloc_ext_init ─────────────────────────────── */
int page_alloc_ext_init(void)
{
    kprintf("[page_alloc] page_alloc_ext_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: page_alloc_bulk ─────────────────────────────── */
int page_alloc_bulk(int flags, int nr, void **pages)
{
    (void)flags;
    (void)nr;
    (void)pages;
    kprintf("[page_alloc] page_alloc_bulk: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: page_free_bulk ─────────────────────────────── */
int page_free_bulk(void **pages, int nr)
{
    (void)pages;
    (void)nr;
    kprintf("[page_alloc] page_free_bulk: not yet implemented\n");
    return -ENOSYS;
}
