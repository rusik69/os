#include "madvise_ext.h"
#include "vmm.h"
#include "pmm.h"
#include "process.h"
#include "printf.h"
#include "errno.h"
#include "string.h"
#include "cpu_topology.h"
#include "ksm.h"

/* ── Software-defined PTE bits (not in vmm.h) ────────────────────────── */
#define PTE_COW      (1ULL << 9)   /* copy-on-write */
#define PTE_LAZY     (1ULL << 10)  /* lazy/demand allocation */
#define PTE_EXECONLY (1ULL << 11)  /* execute-only tracking */
#define PTE_PCD      (1ULL << 4)   /* page cache disable */

/* TLB flush for a single page */
static inline void local_tlb_flush(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ── User page-table walker ───────────────────────────────────────────── */

/*
 * Walk the current process's user page table and apply an operation
 * to each present page in the range [addr, addr + len).
 *
 * The operation receives the physical address, PTE flags, and a pointer
 * to the PTE itself (so it can modify or clear it).
 *
 * Returns 0 on success, or -EINVAL / -EFAULT on error.
 */
typedef void (*page_op_t)(uint64_t phys, uint64_t *pte_ptr, uint64_t virt);

static int walk_user_pages(uint64_t addr, uint64_t len, page_op_t op)
{
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4)
        return -EINVAL;

    /* Address and length must be page-aligned / rounded */
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    uint64_t end = addr + ((len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL));
    if (end < addr || end > USER_VADDR_MAX)
        return -EINVAL;

    uint64_t *pml4 = proc->pml4;

    for (uint64_t v = addr; v < end; v += PAGE_SIZE) {
        int pml4_idx = (v >> 39) & 0x1FF;
        int pdpt_idx = (v >> 30) & 0x1FF;
        int pd_idx   = (v >> 21) & 0x1FF;
        int pt_idx   = (v >> 12) & 0x1FF;

        /* Walk page tables */
        if (!(pml4[pml4_idx] & PTE_PRESENT))
            continue; /* not mapped — skip */
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT))
            continue;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT))
            continue;

        /* Handle 2MB huge pages */
        if (pd[pd_idx] & PTE_HUGE) {
            uint64_t phys = pd[pd_idx] & PTE_ADDR_MASK;
            uint64_t offset = v & (HUGE_PAGE_SIZE - 1ULL);
            op(phys + offset, &pd[pd_idx], v);
            local_tlb_flush(v);
            /* Skip remaining 4K pages within this huge page */
            uint64_t remaining = HUGE_PAGE_SIZE - (v & (HUGE_PAGE_SIZE - 1ULL));
            v += remaining - PAGE_SIZE; /* loop increment adds PAGE_SIZE */
            continue;
        }

        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT))
            continue;

        uint64_t phys = pt[pt_idx] & PTE_ADDR_MASK;
        op(phys, &pt[pt_idx], v);
        local_tlb_flush(v);
    }

    return 0;
}

/* ── Operation callbacks ──────────────────────────────────────────────── */

/* External reference to the shared zero page used for lazy/demand allocation */
extern uint64_t vmm_zero_page_frame;

/*
 * MADV_DONTNEED: immediately unmap and free physical pages.
 * The address range becomes invalid; subsequent access will fault.
 */
static void op_dontneed(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;

    /* Clear the PTE (unmap) */
    uint64_t old_pte = *pte_ptr;
    *pte_ptr = 0;

    /* Free or unreference the physical frame */
    if (phys && phys != vmm_zero_page_frame) {
        /* If it was a COW page, decrement refcount (frees when it hits 0) */
        if (old_pte & PTE_COW) {
            pmm_unref_frame(phys);
        } else {
            pmm_free_frame(phys);
        }
    }
    /* vmm_zero_page_frame is never freed — it is shared globally */
}

/*
 * MADV_COLD: hint that pages are cold (unlikely to be accessed soon).
 * Clear the accessed and dirty bits so the kernel can reclaim them first
 * under memory pressure.  Also clear the dirty bit to reduce writeback cost.
 * Since we have no swap, we simply unmap and free (like DONTNEED) to reclaim
 * immediately — the hint becomes an action.
 */
static void op_cold(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;
    if (!phys) return;

    /* If it's the shared zero page or still in use, just clear accessed/dirty */
    if (phys == vmm_zero_page_frame) {
        *pte_ptr &= ~(uint64_t)(PTE_ACCESSED | PTE_DIRTY);
        return;
    }

    /* For normal pages: unmap and free immediately (conservative but simple) */
    uint64_t old_pte = *pte_ptr;
    *pte_ptr = 0;

    if (old_pte & PTE_COW) {
        pmm_unref_frame(phys);
    } else {
        pmm_free_frame(phys);
    }
}

/*
 * MADV_PAGEOUT: proactively swap out pages.
 * Without swap support, we treat this like DONTNEED (unmap + free).
 */
static void op_pageout(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;
    if (!phys || phys == vmm_zero_page_frame) return;

    uint64_t old_pte = *pte_ptr;
    *pte_ptr = 0;

    if (old_pte & PTE_COW) {
        pmm_unref_frame(phys);
    } else {
        pmm_free_frame(phys);
    }
}

/*
 * MADV_FREE: lazy freeing — pages become freeable but are not immediately
 * reclaimed.  On next access, if not yet reclaimed, the page is zero-filled.
 * We implement this similarly to DONTNEED (immediate reclaim) for simplicity.
 */
static void op_free(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;
    if (!phys || phys == vmm_zero_page_frame) return;

    uint64_t old_pte = *pte_ptr;
    *pte_ptr = 0;

    if (old_pte & PTE_COW) {
        pmm_unref_frame(phys);
    } else {
        pmm_free_frame(phys);
    }
}

/*
 * MADV_MERGEABLE: mark pages as candidates for KSM merging.
 * Clear accessed/dirty bits so the KSM scanner will consider them,
 * and register the physical page with the KSM subsystem for scanning.
 */
static void op_mergeable(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;
    if (!phys || phys == vmm_zero_page_frame)
        return;

    /* Clear accessed/dirty to make them visible for KSM scanning */
    *pte_ptr &= ~(uint64_t)(PTE_ACCESSED | PTE_DIRTY);
    /* Ensure the pages are present and user-accessible */
    *pte_ptr |= PTE_PRESENT | PTE_USER;

    /* Register the physical page with KSM for scanning and merging.
     * ksm_register_phys() handles deduplication (same page won't be
     * added twice), so it's safe to call on every madvise(). */
    int numa = numa_home_node();
    ksm_register_phys(phys, numa);
}

/*
 * MADV_UNMERGEABLE: unmark pages as KSM mergeable candidates.
 * Unregister the physical pages from KSM scanning so they will no
 * longer be considered for merging.
 */
static void op_unmergeable(uint64_t phys, uint64_t *pte_ptr, uint64_t virt)
{
    (void)virt;
    (void)pte_ptr;
    if (!phys || phys == vmm_zero_page_frame)
        return;

    ksm_unregister_phys(phys);
}

/* ── State ─────────────────────────────────────────────────────────────── */

static int madvise_ext_initialised = 0;

void __init madvise_ext_init(void)
{
    if (madvise_ext_initialised)
        return;
    madvise_ext_initialised = 1;
    kprintf("[OK] madvise_ext: extended madvise operations initialized\n");
}

/* ── Public API ────────────────────────────────────────────────────────── */

int madvise_dontneed(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_dontneed);
}

int madvise_willneed(uint64_t addr, uint64_t len)
{
    struct process *proc = process_get_current();
    if (!proc || !proc->pml4)
        return -EINVAL;

    /* Align address down, round length up */
    uint64_t aligned_addr = addr & ~(PAGE_SIZE - 1ULL);
    uint64_t end = addr + len;
    uint64_t aligned_end = (end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (aligned_end < aligned_addr || aligned_end > USER_VADDR_MAX)
        return -EINVAL;

    uint64_t *pml4 = proc->pml4;
    int prefaulted = 0;

    for (uint64_t v = aligned_addr; v < aligned_end; v += PAGE_SIZE) {
        int pml4_idx = (v >> 39) & 0x1FF;
        int pdpt_idx = (v >> 30) & 0x1FF;
        int pd_idx   = (v >> 21) & 0x1FF;
        int pt_idx   = (v >> 12) & 0x1FF;

        /* Walk page tables */
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
            uint64_t offset = v & (HUGE_PAGE_SIZE - 1ULL);
            uint64_t phys = (pd[pd_idx] & PTE_ADDR_MASK) + offset;
            /* Software prefetch */
            __builtin_prefetch((void *)(uintptr_t)PHYS_TO_VIRT(phys), 0, 3);
            prefaulted++;
            /* Skip remaining 4K pages within this huge page */
            uint64_t remaining = HUGE_PAGE_SIZE - (v & (HUGE_PAGE_SIZE - 1ULL));
            v += remaining - PAGE_SIZE;
            continue;
        }

        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        if (!(pt[pt_idx] & PTE_PRESENT)) {
            /* Page not yet mapped — allocate and map zero page */
            uint64_t new_phys = pmm_alloc_frame();
            if (new_phys) {
                /* Zero the frame */
                memset(PHYS_TO_VIRT(new_phys), 0, PAGE_SIZE);
                /* Map it with the same flags as the surrounding pages */
                uint64_t flags = PTE_PRESENT | PTE_USER | PTE_ACCESSED;
                if (pd[pd_idx] & PTE_WRITE)
                    flags |= PTE_WRITE;
                pt[pt_idx] = new_phys | flags;
                local_tlb_flush(v);
                prefaulted++;
            }
        } else {
            /* Page already present — prefetch */
            uint64_t phys = pt[pt_idx] & PTE_ADDR_MASK;
            __builtin_prefetch((void *)(uintptr_t)PHYS_TO_VIRT(phys), 0, 3);
            prefaulted++;
        }
    }

    return prefaulted > 0 ? 0 : 0; /* Success even if nothing to prefault */
}

int madvise_cold(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_cold);
}

int madvise_pageout(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_pageout);
}

int madvise_free(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_free);
}

int madvise_mergeable(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_mergeable);
}

int madvise_unmergeable(uint64_t addr, uint64_t len)
{
    return walk_user_pages(addr, len, op_unmergeable);
}

/* ── Stub: madvise_remove ─────────────────────────────── */
int madvise_remove(uint64_t addr, size_t len)
{
    (void)addr;
    (void)len;
    kprintf("[madvise] madvise_remove: not yet implemented\n");
    return 0;
}
/* ── Stub: madvise_hugepage ─────────────────────────────── */
int madvise_hugepage(uint64_t addr, size_t len)
{
    (void)addr;
    (void)len;
    kprintf("[madvise] madvise_hugepage: not yet implemented\n");
    return 0;
}
/* ── Stub: madvise_nohugepage ─────────────────────────────── */
int madvise_nohugepage(uint64_t addr, size_t len)
{
    (void)addr;
    (void)len;
    kprintf("[madvise] madvise_nohugepage: not yet implemented\n");
    return 0;
}
