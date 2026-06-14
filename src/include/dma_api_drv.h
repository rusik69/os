#ifndef DMA_API_DRV_H
#define DMA_API_DRV_H

#include "types.h"
#include "dma.h"

/*
 * DMA API driver-level helpers — supplementary DMA operations.
 *
 * This file provides additional DMA utility functions on top of the
 * core DMA API already implemented in kernel/dma_api.c.
 *
 * When IOMMU is available (iommu.h exists), operations are backed by
 * the IOMMU page tables; otherwise they fall back to identity-mapped
 * physical addressing.
 *
 * Core functions (defined in kernel/dma_api.c):
 *   dma_alloc_coherent, dma_free_coherent,
 *   dma_map_single, dma_unmap_single
 *
 * Additional helpers provided here:
 *   dma_sync_single_for_cpu   — Ensure CPU sees device DMA writes
 *   dma_sync_single_for_device — Ensure device sees CPU updates
 *   dma_alloc_coherent_aligned — Allocate with alignment constraint
 */

/* ── Additional DMA helpers (supplemental) ─────────────────────────── */

/**
 * dma_sync_single_for_cpu — Make DMA buffer visible to CPU.
 * Issues a memory barrier to ensure device writes are visible.
 */
void dma_sync_single_for_cpu(uint64_t dma_handle, size_t size,
                              enum dma_data_direction dir);

/**
 * dma_sync_single_for_device — Make CPU updates visible to device.
 * Issues a write barrier before the device accesses the buffer.
 */
void dma_sync_single_for_device(uint64_t dma_handle, size_t size,
                                 enum dma_data_direction dir);

/**
 * dma_alloc_coherent_aligned — Allocate coherent memory with alignment.
 * Like dma_alloc_coherent but guarantees the returned buffer is aligned
 * to @align bytes (must be power of 2).
 */
void *dma_alloc_coherent_aligned(struct pci_device *dev, size_t size,
                                  uint64_t *dma_handle, uint64_t flags,
                                  size_t align);

#endif /* DMA_API_DRV_H */
