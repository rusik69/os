#include "page_idle.h"
#include "pmm.h"
#include "vmm.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"
#include "smp.h"

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
        return 0;

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
        return 0;
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
        return 0;
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
        return 0;

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

/*
 * Read a portion of the idle/accessed bitmap.
 * For each page frame in [start_pfn, start_pfn + nr_pfns), set the
 * corresponding bit in bitmap to 1 if the page is idle (accessed bit
 * clear) or 0 if accessed.
 *
 * bitmap must point to a buffer at least ceil(nr_pfns/8) bytes.
 */
int page_idle_bitmap_read(uint64_t start_pfn, uint64_t nr_pfns, uint8_t *bitmap)
{
    if (!page_idle_initialised)
        return 0;
    if (!bitmap || nr_pfns == 0)
        return -EINVAL;

    uint64_t total = pmm_get_total_frames();
    for (uint64_t i = 0; i < nr_pfns; i++) {
        uint64_t pfn = start_pfn + i;
        if (pfn >= total)
            break;
        uint64_t phys = pfn * PAGE_SIZE;
        int idle = page_idle_is_idle(phys);
        if (idle != 0 && idle != -ENOSYS && idle != -EINVAL) {
            /* Mark as idle: set bit */
            bitmap[i / 8] |= (uint8_t)(1 << (i % 8));
        } else {
            /* Mark as accessed: clear bit */
            bitmap[i / 8] &= (uint8_t)~(1 << (i % 8));
        }
    }

    return 0;
}

/*
 * Write a portion of the idle/accessed bitmap.
 * For each page whose corresponding bit in bitmap is 1, clear the
 * hardware accessed bit (mark page as idle).
 */
int page_idle_bitmap_write(uint64_t start_pfn, uint64_t nr_pfns, const uint8_t *bitmap)
{
    if (!page_idle_initialised)
        return 0;
    if (!bitmap || nr_pfns == 0)
        return -EINVAL;

    uint64_t total = pmm_get_total_frames();
    for (uint64_t i = 0; i < nr_pfns; i++) {
        uint64_t pfn = start_pfn + i;
        if (pfn >= total)
            break;
        if (bitmap[i / 8] & (uint8_t)(1 << (i % 8))) {
            /* Bit is set: clear the accessed bit on this page */
            uint64_t phys = pfn * PAGE_SIZE;
            page_idle_clear(phys);
        }
    }

    return 0;
}

/* ── page_idle_clear_pte_refs_many ───────────────────────────────────── */
/*
 * For each PFN in the array, walk all user process page tables and clear
 * the accessed (PTE_ACCESSED) bit in every PTE that maps that page frame.
 * This allows the page-idle subsystem to detect further accesses.
 *
 * Returns 0 on success, negative on error.
 */
int page_idle_clear_pte_refs_many(uint64_t *pfns, int nr_pfns)
{
    if (!pfns || nr_pfns <= 0)
        return -EINVAL;

    /* Iterate over all processes and walk their page tables. */
    struct process *table = process_get_table();
    if (!table)
        return 0;

    for (int pid = 0; pid < PROCESS_MAX; pid++) {
        struct process *proc = &table[pid];
        if (proc->state == PROCESS_UNUSED || !proc->pml4)
            continue;
        if (!proc->is_user)
            continue;

        uint64_t *pml4 = proc->pml4;

        for (int p = 0; p < nr_pfns; p++) {
            uint64_t pfn = pfns[p];
            uint64_t phys = pfn * PAGE_SIZE;

            /* Walk the user address space (0 to USER_VADDR_MAX) */
            for (uint64_t vaddr = 0; vaddr < USER_VADDR_MAX; vaddr += PAGE_SIZE) {
                int pml4_idx = (int)((vaddr >> 39) & 0x1FF);
                int pdpt_idx = (int)((vaddr >> 30) & 0x1FF);
                int pd_idx   = (int)((vaddr >> 21) & 0x1FF);
                int pt_idx   = (int)((vaddr >> 12) & 0x1FF);

                if (!(pml4[pml4_idx] & PTE_PRESENT))
                    continue;
                uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
                if (!(pdpt[pdpt_idx] & PTE_PRESENT))
                    continue;
                uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
                if (!(pd[pd_idx] & PTE_PRESENT))
                    continue;

                /* Handle 2MB huge pages */
                if (pd[pd_idx] & PTE_HUGE) {
                    uint64_t huge_phys = pd[pd_idx] & PTE_ADDR_MASK;
                    if (huge_phys <= phys && phys < huge_phys + HUGE_PAGE_SIZE) {
                        pd[pd_idx] &= ~PTE_ACCESSED;
                    }
                    vaddr += HUGE_PAGE_SIZE - PAGE_SIZE;
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
                if (!(pt[pt_idx] & PTE_PRESENT))
                    continue;

                uint64_t pte_phys = pt[pt_idx] & PTE_ADDR_MASK;
                if (pte_phys == phys) {
                    pt[pt_idx] &= ~PTE_ACCESSED;
                }
            }
        }
    }

    return 0;
}

/* ── page_idle_get_page ──────────────────────────────────────────────── */
/*
 * Find an idle page frame by scanning physical memory.
 * Returns the PFN (page frame number) of an idle page, or 0 if none found.
 * If 'cpu' is >= 0, prefer pages from the NUMA node associated with that CPU.
 */
uint64_t page_idle_get_page(int cpu)
{
    if (!page_idle_initialised)
        return 0;

    uint64_t total = pmm_get_total_frames();
    uint64_t start = 0;

    /* Simple NUMA hint: if cpu >= 0, start scanning from that node's region.
     * This is a heuristic — a real implementation would query ACPI SRAT/SLIT. */
    if (cpu >= 0) {
        start = ((uint64_t)cpu * total) / (smp_get_cpu_count() > 0 ? smp_get_cpu_count() : 1);
        if (start >= total)
            start = 0;
    }

    for (uint64_t i = 0; i < total; i++) {
        uint64_t idx = (start + i) % total;
        uint64_t phys = idx * PAGE_SIZE;

        /* Skip reserved areas */
        if (phys < 0x100000)
            continue;

        if (page_idle_is_idle(phys) == 1) {
            return idx;  /* Return PFN */
        }
    }

    return 0; /* No idle page found */
}
