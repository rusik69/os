#ifndef CMA_H
#define CMA_H

#include "types.h"

/* Contiguous Memory Allocator */

/* Maximum number of CMA areas */
#define CMA_MAX_AREAS 8

/* CMA area descriptor */
struct cma_area {
    uint64_t base_pfn;         /* Start PFN */
    uint64_t count;            /* Number of pages */
    uint64_t *bitmap;          /* Allocation bitmap */
    int bitmap_size;           /* Bitmap size in bits */
    char name[32];             /* Area name */
    int initialized;
};

/* Initialize CMA subsystem */
void cma_init(void);

/* Create a CMA area from a range of physical pages */
int cma_create_area(uint64_t base_pfn, uint64_t count, const char *name);

/* Allocate contiguous pages from a named CMA area */
uint64_t cma_alloc(const char *name, size_t count, size_t align);

/* Free pages back to a CMA area */
void cma_free(uint64_t pfn, size_t count);

/* Get total and free page counts for a CMA area */
uint64_t cma_get_total_pages(const char *name);
uint64_t cma_get_free_pages(const char *name);

/* Reserve a default CMA area */
void cma_reserve_default(void);

#endif /* CMA_H */
