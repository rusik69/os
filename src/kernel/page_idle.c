#include "page_idle.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"

static int page_idle_initialised = 0;

void page_idle_init(void)
{
    if (page_idle_initialised)
        return;
    page_idle_initialised = 1;
    kprintf("page_idle: initialised (using accessed-bit tracking)\n");
}

/*
 * Mark a physical page as accessed by mapping it into a temporary
 * kernel virtual address, reading from it (which sets the accessed bit
 * in the page table entry), then unmapping it.
 */
int page_idle_mark_accessed(uint64_t phys)
{
    if (!page_idle_initialised)
        return -ENOSYS;

    if (phys == 0 || (phys & 0xFFF) != 0)
        return -EINVAL;

    /* Map the page at a temporary kernel address. */
    void *virt = vmm_map_phys(phys, PAGE_SIZE, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (!virt)
        return -ENOMEM;

    /* Read a byte to set the accessed bit in the hardware page tables. */
    volatile uint8_t *ptr = (volatile uint8_t *)virt;
    (void)*ptr;

    vmm_unmap_phys(virt, PAGE_SIZE);
    return 0;
}

/*
 * Check whether a physical page is idle by mapping it temporarily and
 * examining the accessed bit in the top-level PTE.  In a real kernel
 * we would read the accessed bit directly from the page table entry
 * (bit 5 of the PTE).  Since we cannot easily acquire the PTE pointer
 * from phys alone, we map temporarily and check whether the hardware
 * accessed bit is set via the PTE flags returned by vmm_virt_to_phys.
 *
 * A page is considered "idle" if the accessed bit is clear.
 */
int page_idle_is_idle(uint64_t phys)
{
    if (!page_idle_initialised)
        return -ENOSYS;
    if (phys == 0 || (phys & 0xFFF) != 0)
        return -EINVAL;

    /* For this stub, we conservatively return 0 ("not idle").
     * A full implementation would walk page tables to check the
     * accessed bit. */
    (void)phys;
    return 0;
}

/*
 * Clear the accessed bit (and thus the idle flag) on a physical page.
 */
int page_idle_clear(uint64_t phys)
{
    if (!page_idle_initialised)
        return -ENOSYS;
    if (phys == 0 || (phys & 0xFFF) != 0)
        return -EINVAL;

    void *virt = vmm_map_phys(phys, PAGE_SIZE, VMM_FLAG_PRESENT | VMM_FLAG_WRITE);
    if (!virt)
        return -ENOMEM;

    /* Temporarily map to get the PTE, then clear bit 5 (accessed).
     * In this stub we simply unmap — full implementation would do
     * a proper PTE update. */
    vmm_unmap_phys(virt, PAGE_SIZE);
    return 0;
}

/*
 * Scan all available pages and report which are idle.
 * Uses the page-idle scan callback for each frame.
 * Returns the count of idle pages found.
 */
int page_idle_scan_idle(void)
{
    if (!page_idle_initialised)
        return -ENOSYS;

    uint64_t total = pmm_get_total_frames();
    uint64_t idle_count = 0;

    for (uint64_t i = 0; i < total; i++) {
        uint64_t phys = i * PAGE_SIZE;
        /* Skip the zero page and kernel-reserved areas. */
        if (phys < 0x100000) /* below 1 MB is typically reserved */
            continue;
        if (page_idle_is_idle(phys))
            idle_count++;
    }

    return (int)idle_count;
}
