#ifndef DMA_H
#define DMA_H

#include "types.h"
#include "pmm.h"
#include "vmm.h"
#include "cma.h"
#include "heap.h"
#include "string.h"

/*
 * DMA API with real contiguous memory allocation from CMA region.
 *
 * Uses the existing CMA (Contiguous Memory Allocator) to allocate
 * physically-contiguous DMA buffers. The buffers are mapped into
 * the kernel's virtual address space for CPU access.
 *
 * CMA region is set up by cma_reserve_default() during boot,
 * typically at the end of physical memory.
 */

/* Direction for dma_map_single */
enum dma_data_direction {
    DMA_TO_DEVICE       = 0,
    DMA_FROM_DEVICE     = 1,
    DMA_BIDIRECTIONAL   = 2,
};

/*
 * Allocate a coherent (non-cacheable) DMA buffer from CMA.
 *
 * Allocates physically-contiguous pages from the default CMA area,
 * then maps them into the kernel high-half VMA space uncacheable.
 *
 * Returns kernel virtual address, writes physical address to *phys.
 * Returns NULL on failure.
 */
static inline void *dma_alloc_coherent(size_t size, uint64_t *phys) {
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0) return NULL;

    /* Allocate contiguous physical pages from CMA */
    uint64_t pfn = cma_alloc("default", num_pages, 1);
    if (!pfn) {
        /* Fallback: allocate individual pages and hope they're contiguous */
        /* For now, just fail */
        return NULL;
    }

    uint64_t phys_addr = pfn * PAGE_SIZE;

    /* Map the physical pages into kernel address space uncacheable */
    void *virt = vmm_map_phys(phys_addr, num_pages * PAGE_SIZE,
                               VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
    if (!virt) {
        cma_free(pfn, num_pages);
        return NULL;
    }

    /* Zero the buffer */
    memset(virt, 0, num_pages * PAGE_SIZE);

    if (phys) *phys = phys_addr;

    return virt;
}

/*
 * Free a coherent DMA buffer allocated with dma_alloc_coherent.
 */
static inline void dma_free_coherent(void *virt, size_t size) {
    if (!virt) return;

    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Unmap from kernel address space */
    vmm_unmap_phys(virt, num_pages * PAGE_SIZE);

    /* Free back to CMA */
    uint64_t phys_addr = (uint64_t)virt - 0xFFFF800000000000ULL;
    uint64_t pfn = phys_addr / PAGE_SIZE;
    cma_free(pfn, num_pages);
}

/*
 * Map a physical address for DMA.
 *
 * Since we use identity-mapped DMA (no IOMMU translation),
 * the bus address is the same as the physical address.
 * In future with IOMMU support, this would establish an
 * IOMMU mapping.
 *
 * Returns the bus address (same as phys_addr in identity mode).
 */
static inline uint64_t dma_map_single(uint64_t phys_addr, size_t size,
                                       enum dma_data_direction dir) {
    (void)size;
    (void)dir;

    /* Flush CPU caches for the buffer to ensure coherency.
     * In a real kernel this would use CLFLUSH or WBINVD.
     * Since we allocate uncacheable, this is a no-op. */
    __asm__ volatile("mfence" ::: "memory");

    return phys_addr;  /* identity mapping (phys = bus address) */
}

/*
 * Unmap a DMA mapping.
 */
static inline void dma_unmap_single(uint64_t bus_addr, size_t size,
                                     enum dma_data_direction dir) {
    (void)bus_addr;
    (void)size;
    (void)dir;

    /* For uncacheable DMA buffers, no cache maintenance needed */
    __asm__ volatile("mfence" ::: "memory");
}

#endif /* DMA_H */
