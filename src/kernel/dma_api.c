/*
 * dma_api.c — IOMMU-backed DMA API
 *
 * Provides coherent and streaming DMA operations with IOMMU
 * address translation for PCI devices.
 *
 * Functions:
 *   dma_alloc_coherent   — Allocate + IOMMU-map contiguous pages
 *   dma_free_coherent    — Unmap + free
 *   dma_map_single       — IOMMU-map an existing buffer
 *   dma_unmap_single     — Tear down an IOMMU mapping
 *
 * When the IOMMU is not enabled (iommu_is_enabled() returns 0),
 * all operations fall back to identity mapping where the DMA bus
 * address equals the physical address.
 */

#define KERNEL_INTERNAL
#include "dma.h"
#include "iommu.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "errno.h"
#include "err.h"

/* ── DMA mask validation ─────────────────────────────────────────── */

/*
 * dma_addr_fits_mask — Check if a DMA address range fits the device's mask.
 * Returns 0 if OK, -EIO if the range exceeds the mask.
 */
static int dma_addr_fits_mask(struct pci_device *dev, uint64_t dma_addr,
                               size_t num_pages, uint64_t mask)
{
    /* No mask constraint — allow everything (64-bit) */
    if (mask == 0)
        return 0;

    uint64_t end = dma_addr + num_pages * PAGE_SIZE - 1;

    /* Check every bit set in the address range is within the mask */
    if ((dma_addr & ~mask) != 0 || (end & ~mask) != 0) {
        kprintf("[DMA] ERROR: dma_addr 0x%llx-0x%llx exceeds mask 0x%016llx "
                "for dev %02x:%02x.%x\n",
                (unsigned long long)dma_addr, (unsigned long long)end,
                (unsigned long long)mask,
                dev->bus, dev->slot, dev->func);
        return -EIO;
    }
    return 0;
}

/* ── Internal helpers ───────────────────────────────────────────── */

/* Convert a kernel virtual address to physical (via the fixed VMA offset) */
static uint64_t dma_virt_to_phys(void *cpu_addr)
{
    return VIRT_TO_PHYS(cpu_addr);
}

/*
 * dma_map_pages_iommu — IOMMU-map a physically-contiguous range.
 * Returns the IOVA base address (same as phys if IOMMU off), or ~0 on error.
 */
static uint64_t dma_map_pages_iommu(struct pci_device *dev,
                                     uint64_t phys_addr, size_t num_pages,
                                     uint64_t iommu_flags)
{
    /* If no IOMMU or no device, use identity mapping */
    if (!dev || !iommu_is_enabled())
        return phys_addr;

    /*
     * Use the physical address itself as the IOVA — a simple identity
     * mapping in the IOMMU page table.  This keeps things simple.
     * For a real OS you would use an IOVA allocator.
     */
    uint64_t iova = phys_addr;

    for (size_t i = 0; i < num_pages; i++) {
        int ret = iommu_map_page(dev,
                                  phys_addr + i * PAGE_SIZE,
                                  iova + i * PAGE_SIZE,
                                  iommu_flags);
        if (ret < 0) {
            /* Unwind on failure */
            for (size_t j = 0; j < i; j++)
                iommu_unmap_page(dev, iova + j * PAGE_SIZE);
            return ~0ULL;
        }
    }

    return iova;
}

/*
 * dma_unmap_pages_iommu — Tear down an IOMMU mapping.
 */
static void dma_unmap_pages_iommu(struct pci_device *dev,
                                   uint64_t iova, size_t num_pages)
{
    if (!dev || !iommu_is_enabled())
        return;

    for (size_t i = 0; i < num_pages; i++)
        iommu_unmap_page(dev, iova + i * PAGE_SIZE);
}

/* ── Public API ─────────────────────────────────────────────────── */

/**
 * dma_alloc_coherent - Allocate and IOMMU-map contiguous DMA-coherent memory
 * @dev: Target PCI device (may be NULL for identity mapping)
 * @size: Number of bytes to allocate
 * @dma_handle: On success, set to the DMA bus address (IOVA) of the buffer
 * @flags: Allocation flags (currently unused)
 *
 * Allocates physically-contiguous pages, maps them uncacheable into the
 * kernel virtual address space, zeros the buffer, and creates an IOMMU
 * mapping for the device.  If the IOMMU is disabled, the DMA handle
 * is identical to the physical address (identity mapping).
 *
 * Return: A kernel virtual address for the buffer, or NULL on failure
 */
void *dma_alloc_coherent(struct pci_device *dev, size_t size,
                          uint64_t *dma_handle, uint64_t flags)
{
    (void)flags;

    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0) {
        if (dma_handle) *dma_handle = 0;
        return NULL;
    }

    size_t alloc_size = num_pages * PAGE_SIZE;

    /*
     * Allocate physically-contiguous pages.
     * For simplicity, allocate one at a time — the PMM's
     * natural allocation order is often contiguous for small
     * allocations.  A real OS would use CMA here.
     */
    uint64_t first_phys = 0;
    uint64_t *pages = (uint64_t *)kmalloc(sizeof(uint64_t) * num_pages);
    if (!pages) {
        if (dma_handle) *dma_handle = 0;
        return NULL;
    }

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t p = pmm_alloc_frame();
        if (!p) {
            /* Free already-allocated pages */
            for (size_t j = 0; j < i; j++)
                pmm_free_frame(pages[j]);
            kfree(pages);
            if (dma_handle) *dma_handle = 0;
            return NULL;
        }
        pages[i] = p;
        if (i == 0)
            first_phys = p;
    }

    /*
     * Check contiguity — we need physically contiguous pages for
     * a single dma_handle.  If pages aren't contiguous, free and fail.
     */
    for (size_t i = 1; i < num_pages; i++) {
        if (pages[i] != pages[i - 1] + PAGE_SIZE) {
            kprintf("[DMA] dma_alloc_coherent: non-contiguous pages "
                    "(got 0x%llx, expected 0x%llx)\n",
                    (unsigned long long)pages[i],
                    (unsigned long long)(pages[i - 1] + PAGE_SIZE));
            for (size_t j = 0; j < num_pages; j++)
                pmm_free_frame(pages[j]);
            kfree(pages);
            if (dma_handle) *dma_handle = 0;
            return NULL;
        }
    }

    kfree(pages);

    /* Map into kernel virtual address space as uncacheable */
    void *virt = vmm_map_phys(first_phys, alloc_size,
                               VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_NOCACHE);
    if (IS_ERR(virt)) {
        for (size_t i = 0; i < num_pages; i++)
            pmm_free_frame(first_phys + i * PAGE_SIZE);
        if (dma_handle) *dma_handle = 0;
        return NULL;
    }

    /* Zero the buffer */
    memset(virt, 0, alloc_size);

    /* IOMMU-map the buffer for the device */
    uint64_t iova = dma_map_pages_iommu(dev, first_phys, num_pages,
                                         IOMMU_READ | IOMMU_WRITE);
    if (iova == ~0ULL) {
        vmm_unmap_phys(virt, alloc_size);
        for (size_t i = 0; i < num_pages; i++)
            pmm_free_frame(first_phys + i * PAGE_SIZE);
        if (dma_handle) *dma_handle = 0;
        return NULL;
    }

    /* Validate against device DMA mask */
    uint64_t cmask = dev ? dev->coherent_dma_mask : 0;
    if (cmask == 0)
        cmask = dev ? dev->dma_mask : 0;
    if (dma_addr_fits_mask(dev, iova, num_pages, cmask) < 0) {
        dma_unmap_pages_iommu(dev, iova, num_pages);
        vmm_unmap_phys(virt, alloc_size);
        for (size_t i = 0; i < num_pages; i++)
            pmm_free_frame(first_phys + i * PAGE_SIZE);
        if (dma_handle) *dma_handle = 0;
        return NULL;
    }

    if (dma_handle)
        *dma_handle = iova;

    return virt;
}

/**
 * dma_free_coherent - Free a DMA-coherent buffer and tear down its mappings
 * @dev: Target PCI device (may be NULL for identity mapping)
 * @size: Original allocation size in bytes
 * @cpu_addr: Kernel virtual address returned by dma_alloc_coherent()
 * @dma_handle: DMA bus address (IOVA) returned by dma_alloc_coherent()
 *
 * Tears down the IOMMU mapping, unmaps the buffer from the kernel
 * virtual address space, and frees the physical pages.  Safe to call
 * with a NULL @cpu_addr.
 */
void dma_free_coherent(struct pci_device *dev, size_t size,
                        void *cpu_addr, uint64_t dma_handle)
{
    if (!cpu_addr)
        return;

    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (num_pages == 0)
        return;

    size_t alloc_size = num_pages * PAGE_SIZE;

    /* Get physical address from virtual */
    uint64_t phys_addr = dma_virt_to_phys(cpu_addr);

    /* Tear down IOMMU mapping */
    dma_unmap_pages_iommu(dev, dma_handle, num_pages);

    /* Unmap from kernel address space */
    vmm_unmap_phys(cpu_addr, alloc_size);

    /* Free physical pages */
    for (size_t i = 0; i < num_pages; i++)
        pmm_free_frame(phys_addr + i * PAGE_SIZE);
}

/**
 * dma_map_single - IOMMU-map an existing buffer for DMA
 * @dev: Target PCI device (may be NULL for identity mapping)
 * @cpu_addr: Kernel virtual address of the buffer to map
 * @size: Number of bytes to map
 * @dir: DMA data direction (TO_DEVICE, FROM_DEVICE, or BIDIRECTIONAL)
 *
 * Maps a buffer into IOMMU space for device DMA access.  Issues a
 * memory barrier (MFENCE) before returning to ensure previous CPU
 * writes are visible to the device.
 *
 * Return: The DMA bus address (IOVA) for the device, or ~0ULL on error
 */
uint64_t dma_map_single(struct pci_device *dev, void *cpu_addr,
                         size_t size, enum dma_data_direction dir)
{
    if (!cpu_addr || size == 0)
        return ~0ULL;

    uint64_t phys_addr = dma_virt_to_phys(cpu_addr);
    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Determine IOMMU access flags from direction */
    uint64_t iommu_flags = 0;
    switch (dir) {
    case DMA_TO_DEVICE:
        iommu_flags = IOMMU_READ;
        break;
    case DMA_FROM_DEVICE:
        iommu_flags = IOMMU_WRITE;
        break;
    case DMA_BIDIRECTIONAL:
        iommu_flags = IOMMU_READ | IOMMU_WRITE;
        break;
    default:
        return -EIO;
    }

    /*
     * Ensure cache coherency before the device accesses the buffer.
     * For now: a full memory barrier.  A real OS would use CLFLUSH
     * or WBINVD if the buffer is cacheable.
     */
    __asm__ volatile("mfence" ::: "memory");

    uint64_t iova = dma_map_pages_iommu(dev, phys_addr, num_pages, iommu_flags);
    if (iova != ~0ULL && dev && dev->dma_mask != 0) {
        if (dma_addr_fits_mask(dev, iova, num_pages, dev->dma_mask) < 0) {
            dma_unmap_pages_iommu(dev, iova, num_pages);
            return ~0ULL;
        }
    }
    return iova;
}

/**
 * dma_unmap_single - Tear down an IOMMU mapping created by dma_map_single()
 * @dev: Target PCI device (may be NULL for identity mapping)
 * @dma_handle: DMA bus address (IOVA) to unmap
 * @size: Original mapped size in bytes
 * @dir: DMA data direction (same as passed to dma_map_single())
 *
 * Removes the IOMMU mapping for the given DMA handle.  Safe to call
 * with a @dma_handle of ~0ULL.
 */
void dma_unmap_single(struct pci_device *dev, uint64_t dma_handle,
                       size_t size, enum dma_data_direction dir)
{
    (void)dir;

    if (dma_handle == ~0ULL)
        return;

    size_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    dma_unmap_pages_iommu(dev, dma_handle, num_pages);

    /*
     * Ensure the device's DMA writes are visible to the CPU.
     */
    kprintf("[DMA] dma_alloc_coherent: non-contiguous pages "
            "fallback not implemented\\n");
}
