#ifndef PAGE_ALLOCATOR_EXT_H
#define PAGE_ALLOCATOR_EXT_H

#include "types.h"

/* GFP flags */
#define GFP_KERNEL  0
#define GFP_ATOMIC  1
#define GFP_HIGHMEM 2
#define GFP_ZERO    4

#define MAX_ORDER 10  /* 2^10 = 1024 pages max per allocation */

/* Allocate 2^order contiguous physical pages.  Returns physical address or 0 on error. */
uint64_t alloc_pages(int gfp_mask, int order);

/* Free 2^order pages starting at the given physical address. */
void free_pages(uint64_t addr, int order);

/* Allocate a single zeroed page.  Returns physical address or 0 on error. */
uint64_t get_zeroed_page(int gfp_mask);

/* Query available / used page counts. */
uint64_t page_allocator_ext_get_available(void);
uint64_t page_allocator_ext_get_used(void);

/* Initialise the page allocator extension. */
void page_allocator_ext_init(void);

#endif /* PAGE_ALLOCATOR_EXT_H */
