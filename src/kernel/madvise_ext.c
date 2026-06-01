#include "madvise_ext.h"
#include "vmm.h"
#include "kernel.h"
#include "printf.h"
#include "errno.h"

static int madvise_ext_initialised = 0;

void madvise_ext_init(void)
{
    if (madvise_ext_initialised)
        return;
    madvise_ext_initialised = 1;
    kprintf("madvise_ext: initialised extended madvise operations\n");
}

/*
 * Walk the user page table for the range [addr, addr+len) and apply
 * an operation to each page.  The operation receives the physical address
 * of each present page and a pointer to the PTE flags.
 */
typedef void (*madvise_page_op_t)(uint64_t phys, uint64_t *pte_flags);

static int madvise_walk_range(uint64_t addr, uint64_t len, madvise_page_op_t op)
{
    if (len == 0)
        return -EINVAL;
    if (addr + len < addr)
        return -EINVAL;

    /* Kernel-private helper — in production this would walk the
     * current process's page table using vmm_virt_to_phys for each
     * page-aligned region.  For now we iterate over page-aligned
     * chunks. */
    uint64_t page_addr = addr & ~(PAGE_SIZE - 1);
    uint64_t end = addr + len;

    while (page_addr < end) {
        uint64_t phys;
        int ret = vmm_virt_to_phys(page_addr, &phys);
        if (ret == 0 && phys != 0) {
            uint64_t flags = VMM_FLAG_PRESENT; /* simplified */
            op(phys, &flags);
        }
        page_addr += PAGE_SIZE;
    }
    return 0;
}

/* --- Per-operation callbacks --- */

static void op_dontneed(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: unmap the page, free the physical frame,
     * zero the PTE. */
}

static void op_willneed(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: prefetch / fault the page in. */
}

static void op_cold(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: clear accessed bit, hint to page reclaim. */
}

static void op_pageout(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: swap out the page. */
}

static void op_free(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: unmap, free, zero. */
}

static void op_mergeable(uint64_t phys, uint64_t *pte_flags)
{
    (void)phys;
    (void)pte_flags;
    /* In a full implementation: mark the page as mergeable for KSM. */
}

/* --- Public API --- */

int madvise_dontneed(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_dontneed);
}

int madvise_willneed(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_willneed);
}

int madvise_cold(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_cold);
}

int madvise_pageout(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_pageout);
}

int madvise_free(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_free);
}

int madvise_mergeable(uint64_t addr, uint64_t len)
{
    if (!madvise_ext_initialised)
        return -ENOSYS;
    return madvise_walk_range(addr, len, op_mergeable);
}
