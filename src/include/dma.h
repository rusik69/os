#ifndef DMA_H
#define DMA_H

#include "types.h"
#include "pmm.h"
#include "vmm.h"
#include "heap.h"
#include "string.h"

/*
 * DMA API with IOMMU-backed DMA mapping.
 *
 * Provides coherent and streaming DMA operations that use the IOMMU
 * for device-address translation.  When the IOMMU is not enabled,
 * fall back to identity-mapped (physical = bus address) DMA.
 *
 * Coherent allocations:
 *   dma_alloc_coherent() — allocate physically-contiguous pages,
 *     IOMMU-map them, and return both CPU virtual address and a
 *     DMA bus address (dma_handle) for the device.
 *
 * Streaming mappings:
 *   dma_map_single() — IOMMU-map an existing buffer for device DMA.
 *   dma_unmap_single() — tear down an IOMMU mapping.
 */

/* DMA_BIT_MASK(n) — create a DMA address mask for n address bits */
#define DMA_BIT_MASK(n)  (((n) == 64) ? ~0ULL : ((1ULL << (n)) - 1))

/* Forward declaration for PCI device (included from pci.h) */
struct pci_device;

/* Direction for dma_map_single / dma_unmap_single */
enum dma_data_direction {
    DMA_TO_DEVICE       = 0,
    DMA_FROM_DEVICE     = 1,
    DMA_BIDIRECTIONAL   = 2,
};

/*
 * dma_alloc_coherent — Allocate a coherent DMA buffer.
 *
 * Allocates physically-contiguous pages, maps them into the
 * kernel virtual address space (uncacheable for coherency),
 * and optionally establishes an IOMMU mapping for the device.
 *
 * @dev:         PCI device (may be NULL; if NULL, identity-map only)
 * @size:        Desired buffer size (rounded up to page boundary)
 * @dma_handle:  Output: DMA bus address (IOVA) for the device to use
 * @flags:       Reserved for future use (0 for now)
 *
 * Returns kernel virtual address on success, NULL on failure.
 */
void *dma_alloc_coherent(struct pci_device *dev, size_t size,
                         uint64_t *dma_handle, uint64_t flags);

/*
 * dma_free_coherent — Free a coherent DMA buffer.
 *
 * Unmaps the buffer from the kernel virtual address space,
 * tears down the IOMMU mapping (if any), and returns the
 * physical pages to the system.
 *
 * @dev:         PCI device (may be NULL)
 * @size:        Original buffer size (must match allocation)
 * @cpu_addr:    Kernel virtual address returned by dma_alloc_coherent
 * @dma_handle:  DMA bus address returned by dma_alloc_coherent
 */
void dma_free_coherent(struct pci_device *dev, size_t size,
                       void *cpu_addr, uint64_t dma_handle);

/*
 * dma_map_single — Map a buffer for DMA.
 *
 * Establishes an IOMMU mapping for an existing kernel buffer
 * so the device can access it via DMA.
 *
 * @dev:         PCI device (may be NULL; identity-map if NULL)
 * @cpu_addr:    Kernel virtual address of the buffer
 * @size:        Size of the buffer in bytes
 * @dir:         DMA direction (TO/FROM/BIDIRECTIONAL)
 *
 * Returns the DMA bus address (IOVA) for the device, or ~0 on error.
 */
uint64_t dma_map_single(struct pci_device *dev, void *cpu_addr,
                        size_t size, enum dma_data_direction dir);

/*
 * dma_unmap_single — Unmap a DMA buffer.
 *
 * Tears down the IOMMU mapping previously established by
 * dma_map_single().
 *
 * @dev:         PCI device (may be NULL)
 * @dma_handle:  DMA bus address returned by dma_map_single
 * @size:        Size of the buffer in bytes
 * @dir:         DMA direction (must match map call)
 */
void dma_unmap_single(struct pci_device *dev, uint64_t dma_handle,
                      size_t size, enum dma_data_direction dir);

/*
 * dma_set_mask — Set the DMA addressing mask for a device.
 *
 * Sets the DMA addressing capability for streaming mappings.
 * The mask should be derived from DMA_BIT_MASK(n) where n is
 * the number of bits the device can address (e.g., 32 for 4GB).
 *
 * @dev:   PCI device
 * @mask:  DMA address mask (e.g., DMA_BIT_MASK(32))
 *
 * Returns 0 on success or -EIO if the mask is too restrictive
 * for the platform's DMA addressing capabilities.
 */
int dma_set_mask(struct pci_device *dev, uint64_t mask);

/*
 * dma_set_coherent_mask — Set the coherent DMA addressing mask.
 *
 * Same as dma_set_mask but for coherent allocations.
 *
 * @dev:   PCI device
 * @mask:  DMA address mask (e.g., DMA_BIT_MASK(32))
 *
 * Returns 0 on success or -EIO if the mask is too restrictive.
 */
int dma_set_coherent_mask(struct pci_device *dev, uint64_t mask);

#endif /* DMA_H */
