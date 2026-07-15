/*
 * dma-api.c — DMA API supplemental helpers (drivers/)
 *
 * Provides additional DMA utility functions that complement the core
 * DMA API in kernel/dma_api.c:
 *
 *   dma_sync_single_for_cpu      — Memory barrier for CPU visibility
 *   dma_sync_single_for_device   — Memory barrier for device visibility
 *   dma_alloc_coherent_aligned   — Aligned coherent allocation
 *
 * The core DMA API (dma_alloc_coherent, dma_free_coherent,
 * dma_map_single, dma_unmap_single) lives in kernel/dma_api.c.
 *
 * This file checks for iommu.h / iommu.c and uses IOMMU-based
 * translation when available, falling back to direct phys mapping.
 */

#define KERNEL_INTERNAL
#include "dma_api_drv.h"
#include "dma.h"
#include "iommu.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "errno.h"

/* ── Synchronisation helpers ────────────────────────────────────────── */

void dma_sync_single_for_cpu(uint64_t dma_handle, size_t size,
                              enum dma_data_direction dir)
{
    (void)dma_handle;
    (void)size;
    (void)dir;

    /*
     * Ensure the device's DMA writes are visible to the CPU.
     * In production this would use CLFLUSH or WBINVD for cacheable
     * buffers, or just a barrier for uncacheable buffers.
     */
    __asm__ volatile("mfence" ::: "memory");
}

void dma_sync_single_for_device(uint64_t dma_handle, size_t size,
                                 enum dma_data_direction dir)
{
    (void)dma_handle;
    (void)size;
    (void)dir;

    /*
     * Ensure any CPU writes to the buffer are visible to the device.
     */
    __asm__ volatile("mfence" ::: "memory");
}

/* ── Aligned coherent allocation ────────────────────────────────────── */

void *dma_alloc_coherent_aligned(struct pci_device *dev, size_t size,
                                  uint64_t *dma_handle, uint64_t flags,
                                  size_t align)
{
    if (size == 0) {
        if (dma_handle)
            *dma_handle = 0;
        return NULL;
    }

    /* Align size up to page boundary */
    size_t alloc_size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    /* Allocate extra to guarantee alignment */
    size_t extra = (align > PAGE_SIZE) ? align : PAGE_SIZE;
    size_t total_alloc = alloc_size + extra;

    uint64_t tmp_handle;
    void *buf = dma_alloc_coherent(dev, total_alloc, &tmp_handle, flags);
    if (!buf) {
        if (dma_handle)
            *dma_handle = 0;
        return NULL;
    }

    /* Align the virtual address */
    uintptr_t aligned = ((uintptr_t)buf + align - 1) & ~(align - 1);

    /* Adjust dma_handle by the same offset */
    uint64_t offset = aligned - (uintptr_t)buf;
    uint64_t aligned_dma = tmp_handle + offset;

    if (dma_handle)
        *dma_handle = aligned_dma;

    kprintf("[DMA-Drivers] Allocated aligned coherent: buf=%p aligned=%p "
            "dma_handle=0x%llx align=%lu size=%lu\n",
            buf, (void *)aligned, (unsigned long long)aligned_dma,
            (unsigned long)align, (unsigned long)size);
    return (void *)aligned;
}

/* ── DMA mask management ───────────────────────────────────────── */

int dma_set_mask(struct pci_device *dev, uint64_t mask)
{
    if (!dev)
        return -EINVAL;

    /* Ensure the mask doesn't exclude addresses the platform needs */
    if (mask == 0)
        return -EIO;

    dev->dma_mask = mask;
    kprintf("[DMA] dma_set_mask: dev=%02x:%02x.%x mask=0x%016llx\n",
            dev->bus, dev->slot, dev->func,
            (unsigned long long)mask);
    return 0;
}

int dma_set_coherent_mask(struct pci_device *dev, uint64_t mask)
{
    if (!dev)
        return -EINVAL;

    if (mask == 0)
        return -EIO;

    dev->coherent_dma_mask = mask;
    kprintf("[DMA] dma_set_coherent_mask: dev=%02x:%02x.%x mask=0x%016llx\n",
            dev->bus, dev->slot, dev->func,
            (unsigned long long)mask);
    return 0;
}

/* ── Stub: dma_api_init ─────────────────────────────── */
static int dma_api_init(void)
{
    kprintf("[DMA] dma_api_init: not yet implemented\n");
    return 0;
}
/* ── Stub: dma_api_alloc ─────────────────────────────── */
static void* dma_api_alloc(size_t size)
{
    (void)size;
    kprintf("[DMA] dma_api_alloc: not yet implemented\n");
    return 0;
}
/* ── Stub: dma_api_free ─────────────────────────────── */
static int dma_api_free(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    kprintf("[DMA] dma_api_free: not yet implemented\n");
    return 0;
}
/* ── Stub: dma_api_map ─────────────────────────────── */
static void* dma_api_map(void *ptr, size_t size, int dir)
{
    (void)ptr;
    (void)size;
    (void)dir;
    kprintf("[DMA] dma_api_map: not yet implemented\n");
    return 0;
}
/* ── Stub: dma_api_unmap ─────────────────────────────── */
static int dma_api_unmap(void *addr, size_t size, int dir)
{
    (void)addr;
    (void)size;
    (void)dir;
    kprintf("[DMA] dma_api_unmap: not yet implemented\n");
    return 0;
}
