#ifndef DMA_H
#define DMA_H

#include "types.h"

/* Direction for dma_map_single */
enum dma_data_direction {
    DMA_TO_DEVICE       = 0,
    DMA_FROM_DEVICE     = 1,
    DMA_BIDIRECTIONAL   = 2,
};

/* Allocate a coherent (non-cacheable) DMA buffer.
   Returns virtual address, writes physical address to *phys.
   Uses identity-mapped pages — no IOMMU emulation. */
static inline void *dma_alloc_coherent(size_t size, uint64_t *phys) {
    (void)size;
    if (phys) *phys = 0;
    return NULL;  /* stub: returns NULL — real alloc would get contiguous phys pages */
}

/* Free a coherent DMA buffer. */
static inline void dma_free_coherent(void *virt, size_t size) {
    (void)virt;
    (void)size;
}

/* Map a physical address for DMA (identity mapping, no IOMMU).
   Returns the bus address (same as phys_addr in identity mode). */
static inline uint64_t dma_map_single(uint64_t phys_addr, size_t size,
                                       enum dma_data_direction dir) {
    (void)size;
    (void)dir;
    return phys_addr;  /* identity mapping */
}

/* Unmap a DMA mapping. */
static inline void dma_unmap_single(uint64_t bus_addr, size_t size,
                                     enum dma_data_direction dir) {
    (void)bus_addr;
    (void)size;
    (void)dir;
}

#endif /* DMA_H */
