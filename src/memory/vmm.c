#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "printf.h"
#include "smp.h"
#include "spinlock.h"
#include "thp.h"
#include "export.h"
#include "bug.h"
#include "err.h"
#include "heap.h"

/* Verify our fundamental page size assumption at compile time */
_Static_assert(PAGE_SIZE == 4096, "PAGE_SIZE must be 4096");

/* Page table entries on x86-64 are 8 bytes (uint64_t) */
_Static_assert(sizeof(uint64_t) == 8, "Page table entry / uint64_t must be 8 bytes");

/* NX support status — defined in this file, exported for nx_enforce */
int nx_enabled = 0;

/* If SMP is enabled, use IPI-based TLB shootdown; otherwise local invlpg */
static inline void tlb_flush(uint64_t addr) {
    smp_tlb_shootdown(&addr, 1);
}

/* ── x86-64 Page Table Entry (PTE) constants ──────────────────────────
 * These correspond to the hardware page-table entry bit layout.
 * Now defined in vmm.h to share with KPTI and other subsystems.
 *
 * Software-defined bits (available to OS in bits 9, 10, 11): */
#define PTE_COW      (1ULL << 9)   /* software bit: copy-on-write */
#define PTE_LAZY     (1ULL << 10)  /* software bit: lazy/demand allocation */
#define PTE_EXECONLY (1ULL << 11)  /* software bit: execute-only tracking */
#define PTE_PCD      (1ULL << 4)   /* page cache disable */

#ifndef FEATURE_NX_SUPPORTED
#define FEATURE_NX_SUPPORTED 1
#endif

/* Check if NX is supported via CPUID */
/* (nx_enabled is defined below and exported for nx_enforce) */

/* Initialize NX support detection */
void __init vmm_nx_init(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000001));
    nx_enabled = (edx & (1u << 20)) ? 1 : 0; /* bit 20 = NX */
    if (!nx_enabled) {
        kprintf("[WARN] NX not supported by CPU\n");
    }
}

/* NX checking is now handled by nx_enforce.c — the per-PTE helper below
 * was a preliminary implementation that has been superseded. */

/* Per-process PML4-based NX check for user page table walks.
 * Returns 1 if the access is allowed, 0 if NX violation (should raise PF).
 * 'write'=1 for write access, 'exec'=1 for instruction fetch. */
int vmm_check_nx(uint64_t *pml4, uint64_t virt, int write, int exec) {
    (void)write;
    if (!nx_enabled || !exec) return 1;

    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 1;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 1;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return 1;
    /* Check NX on PDE */
    if (pd[pd_idx] & PTE_NX) return 0;

    if (pd[pd_idx] & PTE_HUGE) return 1; /* 2MB page, NX already checked */

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
    if (!(pt[pt_idx] & PTE_PRESENT)) return 1;
    /* Check NX on PTE */
    if (pt[pt_idx] & PTE_NX) return 0;

    return 1;
}

uint64_t *kernel_pml4;

/* Spinlock protecting the kernel page table (kernel_pml4) against
 * concurrent walks and modifications on SMP systems.
 * Acquire with irqsave/disables because page-table walks can occur
 * in interrupt context (e.g., page-fault handler resolving COW). */
static spinlock_t vmm_page_table_lock = SPINLOCK_INIT;

/* Shared zero page for demand/lazy allocation */
uint64_t vmm_zero_page_frame = 0;

/* VM statistics counters */
uint64_t vm_pgalloc = 0;
uint64_t vm_pgfree = 0;
uint64_t vm_pgfault = 0;
uint64_t vm_pgmajfault = 0;
uint64_t vm_pgswapin = 0;
uint64_t vm_pgswapout = 0;
uint64_t vm_pgin = 0;
uint64_t vm_pgout = 0;
uint64_t vm_hugepages = 0;   /* number of 2MB huge pages allocated */

/* Memory overcommit accounting */
uint64_t vmm_committed_bytes = 0;

int vmm_get_committed(void) { return (int)(__sync_fetch_and_add(&vmm_committed_bytes, 0) / PAGE_SIZE); }
int vmm_commit(uint64_t bytes) {
    uint64_t old = __sync_fetch_and_add(&vmm_committed_bytes, bytes);
    if (old + bytes > VMM_OVERCOMMIT_LIMIT) {
        __sync_fetch_and_sub(&vmm_committed_bytes, bytes);  /* rollback */
        return -ENOMEM;
    }
    return 0;
}
void vmm_uncommit(uint64_t bytes) {
    __sync_fetch_and_sub(&vmm_committed_bytes, bytes);
}

/* Page invalidation */
static inline void invlpg(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

static inline uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static inline void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val) : "memory");
}

static uint64_t *get_or_create_table(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) return ERR_PTR(-ENOMEM);
        uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt, 0, PAGE_SIZE);
        table[index] = frame | flags | PTE_PRESENT | PTE_WRITE;
        return virt;
    }
    /* If the entry is a 2MB huge page, split it into 512 × 4KB entries. */
    if (table[index] & PTE_HUGE) {
        uint64_t huge = table[index];
        uint64_t pt_phys = pmm_alloc_frame();
        if (unlikely(!pt_phys)) return ERR_PTR(-ENOMEM);
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);
        uint64_t base  = huge & 0x000FFFFFFFE00000ULL;
        /* Propagate full flag bits 0-11 (hw + software) and NX (bit 63)
         * from the original huge-page PDE to each 4KB PTE.
         * The PS/HUGE bit is cleared — this is now a 4KB leaf PTE. */
        uint64_t pflags = (huge & 0xFFF) & ~(uint64_t)PTE_HUGE;
        uint64_t pnx    = huge & PTE_NX;
        for (int i = 0; i < 512; i++)
            pt[i] = (base + (uint64_t)i * PAGE_SIZE) | pflags | pnx | PTE_PRESENT;
        table[index] = pt_phys | (flags & 0xFFF) | PTE_PRESENT | PTE_WRITE;
        return pt;
    }
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

void __init vmm_init(void) {
    /* Use current PML4 set up by boot code */
    kernel_pml4 = (uint64_t *)PHYS_TO_VIRT(read_cr3() & PTE_ADDR_MASK);

    /* Keep the identity map (PML4[0]) — the kernel uses PHYS_TO_VIRT for
     * low physical memory access, which goes through PML4[256]. The identity
     * map at PML4[0] is kept for legacy VGA text buffer (0xB8000) access when
     * VGA devices are present. */

    /* Allocate the shared zero page for demand/lazy allocation.
     * A single zero-filled page shared by all lazy mappings via COW.
     * We take an extra refcount so the zero page is never freed — even
     * when all lazy mappings are resolved or destroyed, the page persists
     * for future allocations. */
    vmm_zero_page_frame = pmm_alloc_frame();
    if (vmm_zero_page_frame) {
        memset((void *)PHYS_TO_VIRT(vmm_zero_page_frame), 0, PAGE_SIZE);
        pmm_ref_frame(vmm_zero_page_frame); /* extra ref: permanent pin */
    }
}

/**
 * vmm_map_page - Map a physical page into the kernel's virtual address space
 * @virt: Virtual address to map (must be in kernel space, >= KERNEL_VMA_OFFSET)
 * @phys: Physical address of the page to map (must be page-aligned)
 * @flags: Page table flags (e.g. VMM_FLAG_PRESENT, VMM_FLAG_WRITE, etc.)
 *
 * Maps a 4 KB physical page at the given virtual address in the kernel
 * PML4. Automatically creates intermediate page tables (PDPT, PD, PT) as
 * needed. If a 2 MB huge page is encountered at the target PDE, it is
 * split into 512 × 4 KB entries before mapping. On allocation failure,
 * unwinds any newly created intermediate tables to prevent page-table-page
 * leaks. Issues a TLB flush for the virtual address on success.
 *
 * Context: Any context. May allocate memory for intermediate page tables.
 *          Must not be called from interrupt context if allocation may block.
 * Return: 0 on success, -ENOMEM on page table allocation failure.
 */
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&vmm_page_table_lock, &irq_flags);

    /* Track whether each intermediate table was newly allocated so we can
     * unwind on failure (preventing page-table-page leaks). */
    int pml4_was_empty = !(kernel_pml4[pml4_idx] & PTE_PRESENT);

    uint64_t *pdpt = get_or_create_table(kernel_pml4, pml4_idx, flags);
    if (IS_ERR(pdpt)) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -ENOMEM;
    }

    int pdpt_was_empty = !(pdpt[pdpt_idx] & PTE_PRESENT);

    uint64_t *pd  = get_or_create_table(pdpt, pdpt_idx, flags);
    if (IS_ERR(pd)) {
        if (pml4_was_empty) {
            uint64_t pdpt_phys = kernel_pml4[pml4_idx] & PTE_ADDR_MASK;
            kernel_pml4[pml4_idx] = 0;
            pmm_free_frame(pdpt_phys);
        }
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -ENOMEM;
    }

    int pd_was_empty = !(pd[pd_idx] & PTE_PRESENT);

    uint64_t *pt  = get_or_create_table(pd, pd_idx, flags);
    if (IS_ERR(pt)) {
        if (pdpt_was_empty) {
            uint64_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
            pdpt[pdpt_idx] = 0;
            pmm_free_frame(pd_phys);
        }
        if (pml4_was_empty) {
            uint64_t pdpt_phys = kernel_pml4[pml4_idx] & PTE_ADDR_MASK;
            kernel_pml4[pml4_idx] = 0;
            pmm_free_frame(pdpt_phys);
        }
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -ENOMEM;
    }

    /* Refuse to silently overwrite an existing mapping.
     * Caller must unmap first if they want to remap. */
    if (pt[pt_idx] & PTE_PRESENT) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -EEXIST;
    }

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    tlb_flush(virt);
    spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
    return 0;
}

void vmm_set_range_uncacheable(uint64_t virt, uint64_t size) {
    if (!size) return;
    if (virt + size < virt) return; /* overflow check */
    uint64_t end = virt + size;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&vmm_page_table_lock, &irq_flags);

    while (virt < end) {
        int pml4_idx = (virt >> 39) & 0x1FF;
        int pdpt_idx = (virt >> 30) & 0x1FF;
        int pd_idx   = (virt >> 21) & 0x1FF;

        if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
            goto done;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT))
            goto done;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

        if (pd[pd_idx] & PTE_HUGE) {
            pd[pd_idx] |= PTE_PCD;
            tlb_flush(virt & ~0x1FFFFFULL);
            virt = (virt & ~0x1FFFFFULL) + (2ULL * 1024 * 1024);
            continue;
        }

        if (!(pd[pd_idx] & PTE_PRESENT))
            goto done;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        int pt_idx = (virt >> 12) & 0x1FF;
        if (pt[pt_idx] & PTE_PRESENT) {
            pt[pt_idx] |= PTE_PCD;
            tlb_flush(virt & ~(PAGE_SIZE - 1ULL));
        }
        virt += PAGE_SIZE;
    }

done:
    spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
}

/**
 * vmm_unmap_page - Unmap a page from the kernel's virtual address space
 * @virt: Virtual address to unmap (must be in kernel space)
 *
 * Removes the page table entry for the given virtual address in the kernel
 * PML4. Walks the page table hierarchy (PML4 → PDPT → PD → PT) and clears
 * the final PTE. Issues a TLB flush for the virtual address after unmapping.
 * Does NOT free the underlying physical frame; the caller is responsible
 * for freeing the physical page via pmm_free_frame() if needed.
 *
 * Context: Any context. Must be called with the kernel PML4 active.
 * Return: void.
 */
void vmm_unmap_page(uint64_t virt) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&vmm_page_table_lock, &irq_flags);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) goto done;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) goto done;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) goto done;

    /* If this is a 2MB kernel huge page, clear the PDE directly.
     * Kernel huge pages are set up at boot time (e.g. for the heap);
     * they are not created through vmm_map_page, but must be handled
     * correctly here if they happen to be unmapped. */
    if (pd[pd_idx] & PTE_HUGE) {
        pd[pd_idx] = 0;
        tlb_flush(virt);
        goto done;
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    pt[pt_idx] = 0;
    tlb_flush(virt);

done:
    spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
}

int vmm_virt_to_phys(uint64_t virt, uint64_t *phys) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&vmm_page_table_lock, &irq_flags);

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -EFAULT;
    }
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -EFAULT;
    }
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    /* Check for 2MB huge page */
    if (pd[pd_idx] & (1ULL << 7)) {
        if (phys) *phys = (pd[pd_idx] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return 0;
    }

    if (!(pd[pd_idx] & PTE_PRESENT)) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -EFAULT;
    }
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    if (!(pt[pt_idx] & PTE_PRESENT)) {
        spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
        return -EFAULT;
    }
    if (phys) *phys = (pt[pt_idx] & PTE_ADDR_MASK) + (virt & 0xFFF);
    spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);
    return 0;
}

uint64_t vmm_get_physaddr(uint64_t virt) {
    uint64_t phys = 0;
    vmm_virt_to_phys(virt, &phys);
    return phys;
}

uint64_t *vmm_get_pml4(void) {
    return kernel_pml4;
}

/*
 * Map a region of physical memory in the kernel's high-half VMA space.
 * Returns the virtual address (KERNEL_VMA_OFFSET + phys) on success, NULL on failure.
 * This is the canonical way to map MMIO or temporary physical memory after
 * the identity map is removed.
 */
void *vmm_map_phys(uint64_t phys, uint64_t size, uint64_t flags) {
    if (size == 0) return NULL;
    if (phys + size < phys) return ERR_PTR(-EOVERFLOW); /* overflow check */
    uint64_t start = phys & ~(PAGE_SIZE - 1ULL);
    uint64_t end   = (phys + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t off = 0; off < end - start; off += PAGE_SIZE) {
        uint64_t vaddr = KERNEL_VMA_OFFSET + start + off;
        if (vmm_map_page(vaddr, start + off, flags) < 0)
            return ERR_PTR(-ENOMEM);
    }
    return (void *)(KERNEL_VMA_OFFSET + phys);
}

/* Unmap a region previously mapped with vmm_map_phys. */
void vmm_unmap_phys(void *vaddr, uint64_t size) {
    if (size == 0) return;
    uint64_t va = (uint64_t)(uintptr_t)vaddr;
    if (va + size < va) return; /* overflow check */
    uint64_t start = va & ~(PAGE_SIZE - 1ULL);
    uint64_t end   = (va + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
        vmm_unmap_page(addr);
}

/* ------------------------------------------------------------------ */
/*  Per-process user address space support                             */
/* ------------------------------------------------------------------ */

/**
 * vmm_create_user_pml4 - Create a new user-space PML4 page table
 *
 * Allocates a new PML4 page for a user process and copies the kernel
 * half of the page table (entries 256-511) from the kernel PML4, with
 * the PTE_USER bit cleared for kernel entries. The user half (entries
 * 0-255) is zeroed, ready for user-space mappings.
 *
 * Context: Any context. Allocates one physical frame for the PML4.
 *          Must not be called from interrupt context.
 * Return: Pointer to the new user PML4 (in kernel virtual address space),
 *         or NULL on allocation failure.
 */
uint64_t *vmm_create_user_pml4(void) {
    /* Allocate a new PML4 and copy the upper-half kernel mappings */
    uint64_t frame = pmm_alloc_frame();
    if (unlikely(!frame)) return ERR_PTR(-ENOMEM);
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(frame);
    memset(pml4, 0, PAGE_SIZE);

    /* Copy kernel half (entries 256-511 map kernel space) —
     * hold the page table lock so that kernel_pml4 isn't concurrently
     * modified while we read it. */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&vmm_page_table_lock, &irq_flags);
    for (int i = 256; i < 512; i++)
        pml4[i] = kernel_pml4[i] & ~PTE_USER;
    spinlock_irqsave_release(&vmm_page_table_lock, irq_flags);

    return pml4;
}

static uint64_t *get_or_create_table_in(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) return ERR_PTR(-ENOMEM);
        uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt, 0, PAGE_SIZE);
        table[index] = frame | flags | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

int vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return -EINVAL;
    if (virt >= USER_VADDR_MAX) return -EINVAL;

    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    int pml4_was_empty = !(pml4[pml4_idx] & PTE_PRESENT);

    uint64_t *pdpt = get_or_create_table_in(pml4, pml4_idx, flags);
    if (IS_ERR(pdpt)) return -ENOMEM;

    int pdpt_was_empty = !(pdpt[pdpt_idx] & PTE_PRESENT);

    uint64_t *pd = get_or_create_table_in(pdpt, pdpt_idx, flags);
    if (IS_ERR(pd)) {
        if (pml4_was_empty) {
            uint64_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
            pml4[pml4_idx] = 0;
            pmm_free_frame(pdpt_phys);
        }
        return -ENOMEM;
    }

    int pd_was_empty = !(pd[pd_idx] & PTE_PRESENT);

    uint64_t *pt = get_or_create_table_in(pd, pd_idx, flags);
    if (IS_ERR(pt)) {
        if (pdpt_was_empty) {
            uint64_t pd_phys = pdpt[pdpt_idx] & PTE_ADDR_MASK;
            pdpt[pdpt_idx] = 0;
            pmm_free_frame(pd_phys);
        }
        if (pml4_was_empty) {
            uint64_t pdpt_phys = pml4[pml4_idx] & PTE_ADDR_MASK;
            pml4[pml4_idx] = 0;
            pmm_free_frame(pdpt_phys);
        }
        return -ENOMEM;
    }

    /* Refuse to overwrite an existing user mapping. */
    if (pt[pt_idx] & PTE_PRESENT)
        return -EEXIST;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    return 0;
}

void vmm_unmap_user_page(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_VADDR_MAX) return;
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return;

    /* Handle 2MB huge pages: clear the PDE directly */
    if (pd[pd_idx] & (1ULL << 7)) {
        pd[pd_idx] = 0;
        tlb_flush(virt);
        return;
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
    pt[pt_idx] = 0;
    tlb_flush(virt);
}

static uint64_t *vmm_walk_to_pt(uint64_t *pml4, uint64_t virt,
                                uint64_t *pde_out, uint64_t *pte_out) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return ERR_PTR(-ENOENT);
    if (!(pml4[pml4_idx] & PTE_USER)) return ERR_PTR(-EACCES);
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return ERR_PTR(-ENOENT);
    if (!(pdpt[pdpt_idx] & PTE_USER)) return ERR_PTR(-EACCES);
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) return ERR_PTR(-ENOENT);
    if (!(pd[pd_idx] & PTE_USER)) return ERR_PTR(-EACCES);
    if (pde_out) *pde_out = pd[pd_idx];

    if (pd[pd_idx] & (1ULL << 7)) {
        if (pte_out) *pte_out = pd[pd_idx];
        return ERR_PTR(-ENOENT); /* huge page: not a leaf page table */
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
    if (pte_out) *pte_out = pt[pt_idx];
    return pt;
}

int vmm_user_range_ok(uint64_t *pml4, uint64_t addr, uint64_t len, int write) {
    if (!pml4) return 0;
    if (len == 0) return 1;
    if (addr >= USER_VADDR_MAX) return 0;
    if (len > USER_VADDR_MAX) return 0;
    if (addr + len < addr) return 0;
    if (addr + len > USER_VADDR_MAX) return 0;

    uint64_t cur = addr;
    uint64_t end = addr + len;

    while (cur < end) {
        uint64_t pde = 0;
        uint64_t pte = 0;
        uint64_t *pt = vmm_walk_to_pt(pml4, cur, &pde, &pte);

        if (pde & (1ULL << 7)) {
            if (!(pde & PTE_PRESENT) || !(pde & PTE_USER)) return 0;
            /* COW pages are logically writable — the first write triggers
             * allocation via the existing COW handler. */
            if (write && !(pde & PTE_WRITE) && !(pde & PTE_COW)) return 0;
            cur = (cur & ~0x1FFFFFULL) + 0x200000ULL;
            continue;
        }

        if (IS_ERR(pt)) return 0;
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return 0;
        /* COW pages are logically writable — the first write triggers
         * allocation via the existing COW handler. */
        if (write && !(pte & PTE_WRITE) && !(pte & PTE_COW)) return 0;

        cur = (cur & ~0xFFFULL) + 0x1000ULL;
    }

    return 1;
}

int vmm_user_string_ok(uint64_t *pml4, uint64_t addr, uint64_t max_len) {
    if (!pml4 || addr >= USER_VADDR_MAX || max_len == 0) return 0;
    uint64_t i;
    for (i = 0; i < max_len; i++) {
        uint64_t cur = addr + i;
        if (cur < addr || cur >= USER_VADDR_MAX) return 0;
        /* Check + access atomically: disable interrupts to prevent the page
         * from being unmapped by another process between validation and read. */
        __asm__ volatile("cli");
        if (!vmm_user_range_ok(pml4, cur, 1, 0)) {
            __asm__ volatile("sti");
            return 0;
        }
        if (*(const volatile char *)cur == '\0') {
            __asm__ volatile("sti");
            return 1;
        }
        __asm__ volatile("sti");
    }
    return 0;
}

void vmm_switch_pml4(uint64_t *pml4) {
    uint64_t phys = VIRT_TO_PHYS((uint64_t)pml4);
    write_cr3(phys);
}

/*
 * COW fork clone: share all user pages between parent and child as read-only.
 * Both parent and child PTEs are marked !WRITE | PTE_COW.
 * pmm_ref_frame is called for each shared leaf frame.
 * Returns new child PML4, or NULL on allocation failure.
 */
uint64_t *vmm_clone_user_pml4(uint64_t *src) {
    uint64_t *dst = vmm_create_user_pml4(); /* copies kernel half */
    if (IS_ERR(dst)) return ERR_CAST(dst);

    for (int i = 0; i < 256; i++) {
        if (!(src[i] & PTE_PRESENT)) continue;

        uint64_t *src_pdpt = (uint64_t *)PHYS_TO_VIRT(src[i] & PTE_ADDR_MASK);

        uint64_t dst_pdpt_phys = pmm_alloc_frame();
        if (unlikely(!dst_pdpt_phys)) { vmm_destroy_user_pml4(dst); return NULL; }
        uint64_t *dst_pdpt = (uint64_t *)PHYS_TO_VIRT(dst_pdpt_phys);
        memset(dst_pdpt, 0, PAGE_SIZE);
        dst[i] = dst_pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & PTE_PRESENT)) continue;

            uint64_t *src_pd = (uint64_t *)PHYS_TO_VIRT(src_pdpt[j] & PTE_ADDR_MASK);

            uint64_t dst_pd_phys = pmm_alloc_frame();
            if (unlikely(!dst_pd_phys)) { vmm_destroy_user_pml4(dst); return NULL; }
            uint64_t *dst_pd = (uint64_t *)PHYS_TO_VIRT(dst_pd_phys);
            memset(dst_pd, 0, PAGE_SIZE);
            dst_pdpt[j] = dst_pd_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & PTE_PRESENT)) continue;
                /* Pass 2MB huge pages through with proper refcounting */
                if (src_pd[k] & (1ULL << 7)) {
                    dst_pd[k] = src_pd[k];
                    uint64_t huge_phys = src_pd[k] & 0x000FFFFFFFE00000ULL;
                    for (int l = 0; l < 512; l++)
                        pmm_ref_frame(huge_phys + (uint64_t)l * PAGE_SIZE);
                    continue;
                }

                uint64_t *src_pt = (uint64_t *)PHYS_TO_VIRT(src_pd[k] & PTE_ADDR_MASK);

                uint64_t dst_pt_phys = pmm_alloc_frame();
                if (unlikely(!dst_pt_phys)) { vmm_destroy_user_pml4(dst); return NULL; }
                uint64_t *dst_pt = (uint64_t *)PHYS_TO_VIRT(dst_pt_phys);
                memset(dst_pt, 0, PAGE_SIZE);
                dst_pd[k] = dst_pt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

                for (int l = 0; l < 512; l++) {
                    if (!(src_pt[l] & PTE_PRESENT)) continue;

                    uint64_t frame = src_pt[l] & PTE_ADDR_MASK;
                    uint64_t flags = src_pt[l] & 0xFFF;

                    /* Mark both parent and child as read-only COW */
                    flags = (flags & ~(uint64_t)PTE_WRITE) | PTE_COW;
                    src_pt[l] = frame | flags;
                    dst_pt[l] = frame | flags;
                    pmm_ref_frame(frame); /* shared: increment refcount */
                }
            }
        }
    }
    return dst;
}

void vmm_destroy_user_pml4(uint64_t *pml4) {
    /* Free user-half page table pages (entries 0-255 only) */
    for (int i = 0; i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;
                /* Handle 2MB huge pages */
                if (pd[k] & (1ULL << 7)) {
                    uint64_t huge_phys = pd[k] & 0x000FFFFFFFE00000ULL;
                    /* Free all 512 contiguous frames of the huge page.
                     * Each sub-frame was individually refcounted; pmm_unref_frame
                     * decrements the refcount and frees when it reaches 0. */
                    for (int l = 0; l < 512; l++) {
                        uint64_t sub = huge_phys + (uint64_t)l * PAGE_SIZE;
                        if (sub != vmm_zero_page_frame)
                            pmm_unref_frame(sub);
                    }
                    continue;
                }
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[k] & PTE_ADDR_MASK);
                /* Free each mapped leaf page frame via refcount */
                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        uint64_t p = pt[l] & PTE_ADDR_MASK;
                        /* Don't free the shared zero page — it's a permanent
                         * kernel allocation shared by all processes. */
                        if (p != vmm_zero_page_frame)
                            pmm_unref_frame(p);
                    }
                }
                pmm_free_frame(pd[k] & PTE_ADDR_MASK); /* free PT */
            }
            pmm_free_frame(pdpt[j] & PTE_ADDR_MASK); /* free PD */
        }
        pmm_free_frame(pml4[i] & PTE_ADDR_MASK); /* free PDPT */
    }
    /* Free the PML4 itself */
    uint64_t pml4_phys = VIRT_TO_PHYS((uint64_t)pml4);
    pmm_free_frame(pml4_phys);
}

/*
 * Handle a write fault on a COW page.
 * Returns 1 if the fault was a COW fault and was handled, 0 otherwise.
 */
int vmm_handle_cow_fault(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_VADDR_MAX) return 0;

    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    if (pd[pd_idx] & (1ULL << 7)) return 0; /* 2MB page — skip */
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    uint64_t pte = pt[pt_idx];
    if (!(pte & PTE_PRESENT)) return 0;
    if (!(pte & PTE_COW)) return 0;

    uint64_t old_phys = pte & PTE_ADDR_MASK;

    if (pmm_refcount(old_phys) == 1) {
        /* Last reference — just make writable in place */
        pt[pt_idx] = (pte | PTE_WRITE) & ~PTE_COW;
    } else {
        /* Shared — allocate a private copy */
        uint64_t new_phys = pmm_alloc_frame();
        if (unlikely(!new_phys)) return 0; /* OOM: can't handle */
        memcpy((void *)PHYS_TO_VIRT(new_phys),
               (void *)PHYS_TO_VIRT(old_phys), PAGE_SIZE);
        pmm_unref_frame(old_phys);
        pt[pt_idx] = new_phys | (pte & 0xFFF & ~(uint64_t)PTE_COW) | PTE_WRITE | PTE_PRESENT;
    }
    tlb_flush(virt);
    return 1;
}

/* ── mmap / munmap / mprotect syscall helpers ───────────────────── */

int vmm_page_is_mapped_user(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_VADDR_MAX) return 0;
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    if (pd[pd_idx] & (1ULL << 7)) return 1; /* 2MB page present */
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
    return pt[pt_idx] & PTE_PRESENT;
}

/* Walk user page tables to resolve a virtual address to a physical address.
 * Returns 0 on success with phys set, -1 if not mapped. */
static int vmm_user_virt_to_phys(uint64_t *pml4, uint64_t virt, uint64_t *phys)
{
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    uint64_t pde = 0, pte = 0;
    uint64_t *pt = vmm_walk_to_pt(pml4, virt, &pde, &pte);
    if (pde & PTE_HUGE) {
        if (phys) *phys = (pde & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        return 0;
    }
    if (IS_ERR(pt) || !(pte & PTE_PRESENT)) return -EFAULT;
    if (phys) *phys = (pte & PTE_ADDR_MASK) + (virt & 0xFFF);
    return 0;
}

int vmm_map_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages,
                       uint64_t flags) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */
    if (virt + num_pages * PAGE_SIZE < virt) return -EOVERFLOW; /* add overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -EINVAL;

    size_t i = 0;
    for (i = 0; i < num_pages; i++) {
        uint64_t cur_virt = virt + i * PAGE_SIZE;

        if (flags & VMM_FLAG_LAZY) {
            /* ── Demand / lazy allocation ──────────────────────────────
             * Instead of allocating a physical frame now, map to the
             * shared zero page with read-only + COW.  Reads succeed
             * immediately (returning zeros).  On first write the COW
             * handler allocates a private real page.
             *
             * We strip VMM_FLAG_LAZY and VMM_FLAG_WRITE from the PTE
             * flags, then add VMM_FLAG_COW so the existing COW handler
             * (vmm_handle_cow_fault) processes write faults. */
            uint64_t lazy_flags = flags & ~(VMM_FLAG_LAZY | VMM_FLAG_WRITE);
            lazy_flags |= VMM_FLAG_COW;
            if (vmm_map_user_page(pml4, cur_virt, vmm_zero_page_frame, lazy_flags) < 0)
                goto unwind;
            pmm_ref_frame(vmm_zero_page_frame);
            continue;
        }

        uint64_t phys = pmm_alloc_frame();
        if (unlikely(!phys)) {
            for (size_t j = 0; j < i; j++) {
                uint64_t p;
                vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
                if (vmm_user_virt_to_phys(pml4, virt + j * PAGE_SIZE, &p) == 0 && p && p != vmm_zero_page_frame)
                    pmm_unref_frame(p);
            }
            return -ENOMEM;
        }
        memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (vmm_map_user_page(pml4, cur_virt, phys, flags) < 0) {
            pmm_free_frame(phys);
            for (size_t j = 0; j < i; j++) {
                uint64_t p;
                vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
                if (vmm_user_virt_to_phys(pml4, virt + j * PAGE_SIZE, &p) == 0 && p && p != vmm_zero_page_frame)
                    pmm_unref_frame(p);
            }
            return -ENOMEM;
        }
    }
    return 0;

unwind:
    for (size_t j = 0; j < i; j++) {
        uint64_t p;
        vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
        if (vmm_user_virt_to_phys(pml4, virt + j * PAGE_SIZE, &p) == 0 && p) {
            if (p != vmm_zero_page_frame)
                pmm_unref_frame(p);
        }
    }
    return -ENOMEM;
}

int vmm_unmap_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */
    if (virt + num_pages * PAGE_SIZE < virt) return -EOVERFLOW; /* add overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -EINVAL;

    for (size_t i = 0; i < num_pages; ) {
        uint64_t addr = virt + i * PAGE_SIZE;

        /* ── Detect and handle 2MB huge pages ──────────────────────
         * When the PDE covering this address has the PTE_HUGE bit set,
         * vmm_unmap_user_page will clear the entire PDE. We must unref
         * ALL sub-frames that fall within the requested unmap range
         * before issuing the PDE clear — otherwise the frames outside
         * the first 4KB sub-page leak their refcounts. */
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;

        if ((pml4[pml4_idx] & PTE_PRESENT)) {
            uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
            if ((pdpt[pdpt_idx] & PTE_PRESENT)) {
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
            if ((pd[pd_idx] & PTE_PRESENT) && (pd[pd_idx] & PTE_HUGE)) {
                /* Huge page: unref all sub-frames in the unmap range */
                uint64_t hp_base = pd[pd_idx] & 0x000FFFFFFFE00000ULL;
                size_t start_sub = (addr & (HUGE_PAGE_SIZE - 1)) / PAGE_SIZE;
                size_t remain    = num_pages - i;
                size_t count     = HUGE_PAGE_NFRAMES - start_sub;
                if (count > remain) count = remain;

                for (size_t j = 0; j < count; j++) {
                    uint64_t frame = hp_base + (start_sub + j) * PAGE_SIZE;
                    if (frame != vmm_zero_page_frame)
                        pmm_unref_frame(frame);
                }
                vmm_unmap_user_page(pml4, addr);
                i += count;
                continue;
            }
        }
        }

        /* ── Normal 4KB page path ───────────────────────────────── */
        uint64_t phys = 0;
        if (vmm_user_virt_to_phys(pml4, addr, &phys) == 0 && phys && phys != vmm_zero_page_frame) {
            pmm_unref_frame(phys);
        }
        vmm_unmap_user_page(pml4, addr);
        i++;
    }
    return 0;
}

/* ══════════════════════════════════════════════════════════════════════
 *  Huge page (2MB) support for anonymous mappings
 *
 *  2MB huge pages reduce TLB pressure and page-table walk overhead for
 *  large contiguous anonymous mappings.  When a virtual address is
 *  2MB-aligned and the mapping size is ≥ 2MB, the allocator uses a PDE
 *  with the PTE_HUGE bit (bit 7) instead of creating a full page table.
 *  This means a single TLB entry covers 512× the memory of a 4KB page.
 * ══════════════════════════════════════════════════════════════════════ */

/* ── Helper: map a single 2MB-aligned huge page in a user address space ──
 *
 * The caller MUST provide the physical address of a 2MB-aligned contiguous
 * block (512 × 4KB frames).  The virtual address MUST be 2MB-aligned.
 *
 * This is exposed for use by HugeTLB (MAP_HUGETLB) which pre-allocates its
 * own pool of huge pages.
 *
 * Returns 0 on success, -1 on failure.
 */
int vmm_map_user_hugepage_internal(uint64_t *pml4, uint64_t virt,
                                    uint64_t huge_phys, uint64_t flags) {
    /* Validate alignment constraints */
    if (virt & (HUGE_PAGE_SIZE - 1)) return -EINVAL;
    if (huge_phys & (HUGE_PAGE_SIZE - 1)) return -EINVAL;
    if (virt >= USER_VADDR_MAX) return -EINVAL;
    if (!pml4) return -EINVAL;

    int idx4 = (virt >> 39) & 0x1FF;
    int idx3 = (virt >> 30) & 0x1FF;
    int idx2 = (virt >> 21) & 0x1FF;

    /* Ensure PDPT entry exists (allocate if absent) */
    if (!(pml4[idx4] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) return -ENOMEM;
        uint64_t *virt_pdpt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt_pdpt, 0, PAGE_SIZE);
        pml4[idx4] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[idx4] & PTE_ADDR_MASK);

    /* Ensure PD entry exists (allocate if absent) */
    if (!(pdpt[idx3] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (unlikely(!frame)) return -ENOMEM;
        uint64_t *virt_pd = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt_pd, 0, PAGE_SIZE);
        pdpt[idx3] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[idx3] & PTE_ADDR_MASK);

    /* If there's already a 4K page-table here, we must free it and its
     * page-table page before placing the huge-page PDE. */
    if ((pd[idx2] & PTE_PRESENT) && !(pd[idx2] & PTE_HUGE)) {
        uint64_t pt_phys = pd[idx2] & PTE_ADDR_MASK;
        pmm_free_frame(pt_phys);
    }

    /* Place the huge-page PDE: physical base + flags + HUGE + PRESENT.
     * The physical address mask for a PDE with the HUGE bit is
     * 0x000FFFFFFFE00000 (bits 39:21), which covers 2MB alignment.
     * We take low 12 flag bits from the caller (hw bits 0-8 + software
     * bits 9-11) and explicitly set NX (bit 63) when requested. */
    uint64_t pde = (huge_phys & 0x000FFFFFFFE00000ULL)
                   | (flags & 0xFFF)  /* low 12 flag bits (hw + software) */
                   | PTE_HUGE
                   | PTE_PRESENT
                   | ((flags & VMM_FLAG_NOEXEC) ? PTE_NX : 0);
    pd[idx2] = pde;

    /* Track in THP subsystem */
    thp_track_hugepage(virt, huge_phys);
    vm_hugepages++;

    return 0;
}

/* ── Map user pages using 2MB huge pages where possible ────────────────
 *
 * For anonymous mappings that are large enough (≥ 2MB) and whose virtual
 * address is 2MB-aligned, this function uses 2MB page-table entries
 * instead of 4KB entries, reducing TLB pressure.
 *
 * The mapping strategy:
 *   1. If the start address is not 2MB-aligned, map the leading partial
 *      2MB chunk with regular 4KB pages.
 *   2. Map the fully-aligned middle region with 2MB huge pages.
 *   3. If the end address is not 2MB-aligned, map the trailing partial
 *      2MB chunk with regular 4KB pages.
 *
 * Huge page physical memory is allocated via pmm_alloc_frames(512) which
 * returns contiguous physical frames.  If contiguous allocation fails,
 * we fall back to 4KB pages for that chunk.
 *
 * Returns 0 on success, -1 on failure (partial mappings are NOT undone
 * on failure; the caller must handle cleanup).
 */
int vmm_map_user_huge_pages(uint64_t *pml4, uint64_t virt,
                             size_t num_4k_pages, uint64_t flags) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_4k_pages == 0) return 0;
    if (num_4k_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */

    uint64_t end = virt + (uint64_t)num_4k_pages * PAGE_SIZE;
    if (end < virt || end > USER_VADDR_MAX) return -EINVAL;

    uint64_t cur = virt;

    /* ── Phase 1: Leading partial 2MB chunk (4KB pages) ──────────── */
    uint64_t lead_end = (cur + HUGE_PAGE_SIZE) & ~(HUGE_PAGE_SIZE - 1ULL);
    if (lead_end > end) lead_end = end;

    if (cur < lead_end) {
        size_t lead_pages = (lead_end - cur) / PAGE_SIZE;
        if (vmm_map_user_pages(pml4, cur, lead_pages, flags) < 0)
            return -ENOMEM;
        cur = lead_end;
    }

    /* ── Phase 2: Aligned middle region using 2MB huge pages ─────── */
    while (cur + HUGE_PAGE_SIZE <= end) {
        /* Try to allocate 512 contiguous frames for a huge page */
        uint64_t huge_phys = (uint64_t)pmm_alloc_frames(HUGE_PAGE_NFRAMES);
        if (huge_phys) {
            /* Clear the 2MB region */
            memset((void *)PHYS_TO_VIRT(huge_phys), 0, HUGE_PAGE_SIZE);
            vm_pgalloc += HUGE_PAGE_NFRAMES;

            if (vmm_map_user_hugepage_internal(pml4, cur, huge_phys, flags) < 0) {
                /* Mapping failed — free the frames and fall back to 4KB */
                pmm_free_frames_contiguous(huge_phys, HUGE_PAGE_NFRAMES);
                vm_pgalloc -= HUGE_PAGE_NFRAMES;
                /* Fall through to 4KB fallback below */
            } else {
                /* Successfully mapped as a huge page — advance and continue */
                cur += HUGE_PAGE_SIZE;
                continue;
            }
        }

        /* ── Fallback: use 4KB pages for this 2MB chunk ───────────── */
        if (vmm_map_user_pages(pml4, cur, HUGE_PAGE_NFRAMES, flags) < 0)
            return -ENOMEM;
        cur += HUGE_PAGE_SIZE;
    }

    /* ── Phase 3: Trailing partial 2MB chunk (4KB pages) ─────────── */
    if (cur < end) {
        size_t trail_pages = (end - cur) / PAGE_SIZE;
        if (vmm_map_user_pages(pml4, cur, trail_pages, flags) < 0)
            return -ENOMEM;
    }

    return 0;
}

int vmm_set_user_pages_flags(uint64_t *pml4, uint64_t virt, size_t num_pages,
                             uint64_t new_flags) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */
    if (virt + num_pages * PAGE_SIZE < virt) return -EOVERFLOW; /* add overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -EINVAL;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t addr = virt + i * PAGE_SIZE;
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;
        int pt_idx   = (addr >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) return -EFAULT;

        /* Handle 2MB huge pages: update flags directly in the PDE.
         * The PDE low 9 bits (8:0) contain page flags; bits 9-11 are
         * available for software use (PTE_COW, PTE_EXECONLY, etc.).
         * We preserve the physical address and HUGE bit, replacing the
         * flags but keeping software bits 9-11. */
        if (pd[pd_idx] & (1ULL << 7)) {
            uint64_t pde = pd[pd_idx];
            /* Preserve the physical address base and the HUGE bit */
            uint64_t base = pde & 0x000FFFFFFFE00000ULL;
            uint64_t had_big = pde & PTE_HUGE;
            /* Preserve software bits (9-11) from new_flags */
            uint64_t sw_bits = new_flags & (PTE_COW | PTE_LAZY | PTE_EXECONLY);
            /* Write new flags and re-apply PRESENT + HUGE + software bits.
             * NX (bit 63) is explicitly set/cleared from new_flags. */
            uint64_t hw_flags = (new_flags & 0x1FF) | sw_bits;
            uint64_t nx = (new_flags & VMM_FLAG_NOEXEC) ? PTE_NX : 0;
            pd[pd_idx] = base | hw_flags
                         | had_big
                         | nx
                         | ((new_flags & VMM_FLAG_PRESENT) ? PTE_PRESENT : 0);
            tlb_flush(addr & ~(HUGE_PAGE_SIZE - 1ULL));
            /* Skip the rest of this 2MB region */
            uint64_t remaining = HUGE_PAGE_SIZE / PAGE_SIZE - (i % (HUGE_PAGE_SIZE / PAGE_SIZE));
            if (remaining > 1) {
                i += remaining - 1;
                if (i >= num_pages) break;
            }
            continue;
        }

        if (!(pd[pd_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

        uint64_t pte = pt[pt_idx];
        if (!(pte & PTE_PRESENT)) return -EFAULT;

        /* ── COW-aware flag update ──────────────────────────────────
         * If adding write permission to a COW page, break COW first by
         * allocating a private copy.  Otherwise the shared zero page (or
         * any other COW-shared frame) would become writable in this
         * process's mapping while remaining read-only in the other,
         * violating COW semantics. */
        if ((new_flags & VMM_FLAG_WRITE) && (pte & VMM_FLAG_COW)) {
            uint64_t old_phys = pte & PTE_ADDR_MASK;
            uint64_t new_phys = pmm_alloc_frame();
            if (unlikely(!new_phys)) return -ENOMEM;
            memcpy((void *)PHYS_TO_VIRT(new_phys),
                   (void *)PHYS_TO_VIRT(old_phys), PAGE_SIZE);
            pmm_unref_frame(old_phys);
            /* Preserve NX and EXECONLY from the old PTE if the new
             * flags don't explicitly override them.  The low 12 bits
             * of new_flags become the new low-12 PTE flags. */
            uint64_t preserved = pte & (PTE_NX | PTE_EXECONLY);
            /* If new_flags says executable (no NOEXEC), don't inherit NX */
            if (!(new_flags & VMM_FLAG_NOEXEC))
                preserved &= ~(uint64_t)PTE_NX;
            uint64_t new_pte = (pte & (PTE_ADDR_MASK | 0xFFF))
                               & ~(uint64_t)VMM_FLAG_COW;
            new_pte = (new_pte & ~PTE_ADDR_MASK) | new_phys;
            new_pte = (new_pte & ~(uint64_t)0xFFF)
                      | (new_flags & 0xFFF) | preserved | PTE_PRESENT;
            pt[pt_idx] = new_pte;
            tlb_flush(addr);
            continue;
        }

        /* Preserve physical address, replace flags.
         * Only set PRESENT if the caller asked for it (PROT_NONE clears it).
         * Preserve NX and EXECONLY from the old PTE if the new flags don't
         * explicitly override them. */
        uint64_t preserved = pte & (PTE_NX | PTE_EXECONLY);
        /* If new_flags says executable (no NOEXEC), don't inherit NX */
        if (!(new_flags & VMM_FLAG_NOEXEC))
            preserved &= ~(uint64_t)PTE_NX;
        pt[pt_idx] = (pte & PTE_ADDR_MASK) | (new_flags & 0xFFF) | preserved
                     | ((new_flags & VMM_FLAG_PRESENT) ? PTE_PRESENT : 0);
        tlb_flush(addr);
    }
    return 0;
}

/* ── vmm_lock_user_pages — wire (lock) user pages in memory ──────────────
 * For each present page in the range:
 *   - If the page is COW (lazy zero-page), resolve it first (private copy)
 *   - Set VMM_FLAG_LOCKED in the PTE
 *   - Increment the physical frame refcount via pmm_ref_frame()
 *
 * Returns 0 on success, negative errno on failure (-EFAULT if unmapped,
 * -ENOMEM if COW-break allocation fails).  On error, already-locked pages
 * are NOT unlocked (caller is expected to munlock on error).
 */
int vmm_lock_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */
    if (virt + num_pages * PAGE_SIZE < virt) return -EOVERFLOW; /* add overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -EINVAL;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t addr = virt + i * PAGE_SIZE;
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;
        int pt_idx   = (addr >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) return -EFAULT;

        /* Handle 2MB huge pages */
        if (pd[pd_idx] & PTE_HUGE) {
            uint64_t pde = pd[pd_idx];
            /* Skip if already locked */
            if (pde & VMM_FLAG_LOCKED)
                continue;
            uint64_t base = pde & 0x000FFFFFFFE00000ULL;
            /* Ref all 512 sub-frames */
            for (int j = 0; j < 512; j++)
                pmm_ref_frame(base + (uint64_t)j * PAGE_SIZE);
            /* Set the LOCKED bit in the PDE */
            pd[pd_idx] = pde | VMM_FLAG_LOCKED;
            tlb_flush(addr & ~(HUGE_PAGE_SIZE - 1ULL));
            /* Skip remaining pages in this 2MB region */
            uint64_t remaining = HUGE_PAGE_SIZE / PAGE_SIZE - (i % (HUGE_PAGE_SIZE / PAGE_SIZE));
            if (remaining > 1) {
                i += remaining - 1;
                if (i >= num_pages) break;
            }
            continue;
        }

        if (!(pd[pd_idx] & PTE_PRESENT)) return -EFAULT;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        uint64_t pte = pt[pt_idx];
        if (!(pte & PTE_PRESENT)) return -EFAULT;

        /* Skip if already locked */
        if (pte & VMM_FLAG_LOCKED)
            continue;

        /* Resolve COW pages: allocate private copy if this is a lazy/COW page */
        if (pte & VMM_FLAG_COW) {
            uint64_t old_phys = pte & PTE_ADDR_MASK;
            uint64_t new_phys = pmm_alloc_frame();
            if (unlikely(!new_phys))
                return -ENOMEM;
            memcpy((void *)PHYS_TO_VIRT(new_phys),
                   (void *)PHYS_TO_VIRT(old_phys), PAGE_SIZE);
            pmm_unref_frame(old_phys);
            /* Map the new page: keep all flags except COW, add LOCKED + WRITE */
            uint64_t preserved = pte & (PTE_NX | VMM_FLAG_EXECONLY);
            uint64_t new_pte = (new_phys & PTE_ADDR_MASK)
                             | (pte & 0xFFF & ~(uint64_t)VMM_FLAG_COW)
                             | VMM_FLAG_LOCKED | PTE_WRITE | PTE_PRESENT
                             | preserved;
            pt[pt_idx] = new_pte;
            pmm_ref_frame(new_phys);
            tlb_flush(addr);
            continue;
        }

        /* Normal (non-COW) present page: just set the locked flag and ref */
        uint64_t phys = pte & PTE_ADDR_MASK;
        pt[pt_idx] = pte | VMM_FLAG_LOCKED;
        pmm_ref_frame(phys);
        tlb_flush(addr);
    }
    return 0;
}

/* ── vmm_unlock_user_pages — unwire (unlock) user pages ──────────────────
 * For each page in the range that has VMM_FLAG_LOCKED set:
 *   - Clear VMM_FLAG_LOCKED in the PTE
 *   - Decrement the physical frame refcount via pmm_unref_frame()
 *
 * Pages without VMM_FLAG_LOCKED are silently skipped (Linux semantics:
 * munlock on non-locked pages is a no-op).
 */
int vmm_unlock_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -EINVAL;
    if (num_pages > SIZE_MAX / PAGE_SIZE) return -EOVERFLOW; /* mul overflow */
    if (virt + num_pages * PAGE_SIZE < virt) return -EOVERFLOW; /* add overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -EINVAL;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t addr = virt + i * PAGE_SIZE;
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;
        int pt_idx   = (addr >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) continue;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) continue;

        /* Handle 2MB huge pages */
        if (pd[pd_idx] & PTE_HUGE) {
            uint64_t pde = pd[pd_idx];
            if (!(pde & VMM_FLAG_LOCKED))
                continue;
            uint64_t base = pde & 0x000FFFFFFFE00000ULL;
            /* Unref all 512 sub-frames */
            for (int j = 0; j < 512; j++)
                pmm_unref_frame(base + (uint64_t)j * PAGE_SIZE);
            /* Clear the LOCKED bit */
            pd[pd_idx] = pde & ~VMM_FLAG_LOCKED;
            tlb_flush(addr & ~(HUGE_PAGE_SIZE - 1ULL));
            /* Skip remaining pages in this 2MB region */
            uint64_t remaining = HUGE_PAGE_SIZE / PAGE_SIZE - (i % (HUGE_PAGE_SIZE / PAGE_SIZE));
            if (remaining > 1) {
                i += remaining - 1;
                if (i >= num_pages) break;
            }
            continue;
        }

        if (!(pd[pd_idx] & PTE_PRESENT)) continue;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        uint64_t pte = pt[pt_idx];
        if (!(pte & PTE_PRESENT)) continue;
        if (!(pte & VMM_FLAG_LOCKED)) continue;

        /* Clear the locked flag and unref */
        uint64_t phys = pte & PTE_ADDR_MASK;
        pt[pt_idx] = pte & ~VMM_FLAG_LOCKED;
        pmm_unref_frame(phys);
        tlb_flush(addr);
    }
    return 0;
}

/* ── Walk user page table and count present pages in a range ────────────
 * Count 4KB-equivalent present pages in [start_virt, end_virt).
 * Huge pages (2MB) are counted as 512 × 4KB pages.
 * If dirty_out is non-NULL, receives count of writable/dirty pages.
 * If shared_out is non-NULL, receives count of COW/shared+lazy pages.
 */
uint64_t vmm_count_user_pages_range(uint64_t *pml4,
                                     uint64_t start_virt, uint64_t end_virt,
                                     uint64_t *dirty_out,
                                     uint64_t *shared_out) {
    uint64_t total = 0, dirty = 0, shared = 0;
    if (!pml4 || start_virt >= end_virt || start_virt >= USER_VADDR_MAX)
        goto done;

    /* Clamp range to user space */
    if (end_virt > USER_VADDR_MAX) end_virt = USER_VADDR_MAX;

    /* Align to page boundaries */
    uint64_t va_start = start_virt & ~(uint64_t)0xFFF;
    uint64_t va_end   = (end_virt + 0xFFF) & ~(uint64_t)0xFFF;

    int pml4_lo = (va_start >> 39) & 0x1FF;
    int pml4_hi = (va_end   >> 39) & 0x1FF;
    if (pml4_lo > 255) pml4_lo = 255;
    if (pml4_hi > 255) pml4_hi = 255;

    for (int i = pml4_lo; i <= pml4_hi && i < 256; i++) {
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[i] & PTE_ADDR_MASK);

        int pdpt_lo = (i == pml4_lo) ? ((va_start >> 30) & 0x1FF) : 0;
        int pdpt_hi = (i == pml4_hi) ? ((va_end   >> 30) & 0x1FF) : 511;

        for (int j = pdpt_lo; j <= pdpt_hi; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[j] & PTE_ADDR_MASK);

            int pd_lo = (i == pml4_lo && j == pdpt_lo) ? ((va_start >> 21) & 0x1FF) : 0;
            int pd_hi = (i == pml4_hi && j == pdpt_hi) ? ((va_end   >> 21) & 0x1FF) : 511;

            for (int k = pd_lo; k <= pd_hi; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;

                if (pd[k] & PTE_HUGE) {
                    uint64_t page_base = (((uint64_t)i << 39) | ((uint64_t)j << 30) | ((uint64_t)k << 21));
                    uint64_t page_end  = page_base + HUGE_PAGE_SIZE;
                    /* Only count the overlapping part */
                    uint64_t overlap_start = (page_base > va_start) ? page_base : va_start;
                    uint64_t overlap_end   = (page_end < va_end) ? page_end : va_end;
                    if (overlap_start < overlap_end) {
                        uint64_t overlap_pages = (overlap_end - overlap_start) / PAGE_SIZE;
                        total += overlap_pages;
                        if (pd[k] & PTE_WRITE) dirty += overlap_pages;
                    }
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[k] & PTE_ADDR_MASK);

                int pt_lo = (i == pml4_lo && j == pdpt_lo && k == pd_lo) ? ((va_start >> 12) & 0x1FF) : 0;
                int pt_hi = (i == pml4_hi && j == pdpt_hi && k == pd_hi) ? ((va_end   >> 12) & 0x1FF) : 511;

                for (int l = pt_lo; l <= pt_hi; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    total++;
                    if (pt[l] & PTE_WRITE) dirty++;
                    if (pt[l] & PTE_COW)  shared++;
                    uint64_t phys = pt[l] & PTE_ADDR_MASK;
                    if (phys == vmm_zero_page_frame) shared++;
                }
            }
        }
    }

done:
    if (dirty_out)  *dirty_out  = dirty;
    if (shared_out) *shared_out = shared;
    return total;
}

/* ── Walk user page table and count present pages (for OOM scoring) ── */
uint64_t vmm_count_user_pages(uint64_t *pml4, uint64_t *dirty_out, uint64_t *shared_out) {
    uint64_t total = 0, dirty = 0, shared = 0;

    if (!pml4) goto done;

    for (int i = 0; i < 256; i++) {             /* user half of PML4 */
        if (!(pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[i] & PTE_ADDR_MASK);

        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) continue;
            uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[j] & PTE_ADDR_MASK);

            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) continue;

                if (pd[k] & PTE_HUGE) {
                    /* 2MB huge page = 512 × 4KB pages.  Refcounts are
                     * maintained per-4KB-frame, but the huge page as a
                     * whole is counted. */
                    total += HUGE_PAGE_NFRAMES;
                    if (pd[k] & PTE_WRITE) dirty += HUGE_PAGE_NFRAMES;
                    continue;
                }

                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[k] & PTE_ADDR_MASK);

                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & PTE_PRESENT)) continue;
                    total++;
                    if (pt[l] & PTE_WRITE) dirty++;
                    if (pt[l] & PTE_COW)  shared++;
                    /* Also count lazy pages (shared zero page via COW) as shared */
                    uint64_t phys = pt[l] & PTE_ADDR_MASK;
                    if (phys == vmm_zero_page_frame) shared++;
                }
            }
        }
    }

done:
    if (dirty_out)  *dirty_out  = dirty;
    if (shared_out) *shared_out = shared;
    return total;
}

/* ── Execute-only page check ──────────────────────────────────────
 * Check whether a given user virtual address is mapped with the
 * EXECONLY software tag (bit 11).  Returns 1 if the page is present
 * and tagged EXECONLY, 0 otherwise.
 *
 * Used by the page-fault handler and /proc/self/maps to determine
 * whether a page is execute-only (executable but not readable in
 * software-enforced semantics). */
int vmm_page_is_execonly(uint64_t *pml4, uint64_t virt) {
    if (!pml4 || virt >= USER_VADDR_MAX) return 0;
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;

    if (pd[pd_idx] & PTE_HUGE) {
        return pd[pd_idx] & PTE_EXECONLY;
    }

    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
    if (!(pt[pt_idx] & PTE_PRESENT)) return 0;
    return pt[pt_idx] & PTE_EXECONLY;
}

/* ── Exported symbols for module loading ──────────────────────────── */
EXPORT_SYMBOL(vmm_map_page);
EXPORT_SYMBOL(vmm_unmap_page);
EXPORT_SYMBOL(vmm_get_physaddr);
EXPORT_SYMBOL(vmm_page_is_execonly);

/* ── vmm_alloc — Allocate virtual memory pages ────────────────── */
uint64_t vmm_alloc(uint64_t addr, size_t size, int flags)
{
    if (size == 0)
        return 0;
    /* Align size to page boundary */
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL);
    uint64_t start = addr;
    if (start == 0) {
        /* Auto-select address in user space above typical base */
        start = 0x10000;
    }
    /* Align to page boundary */
    start &= ~(PAGE_SIZE - 1ULL);

    size_t num_pages = size / PAGE_SIZE;
    if (num_pages == 0) num_pages = 1;

    uint64_t vmm_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (flags & 2) vmm_flags |= VMM_FLAG_WRITE;   /* PROT_WRITE */
    if (flags & 1) vmm_flags |= VMM_FLAG_PRESENT;  /* PROT_READ */
    if (!(flags & 4)) vmm_flags |= VMM_FLAG_NOEXEC; /* no PROT_EXEC -> NX */

    /* Map pages one by one using the kernel page table */
    for (size_t i = 0; i < num_pages; i++) {
        uint64_t phys = pmm_alloc_frame();
        if (unlikely(!phys)) {
            /* Unwind on failure: free each frame before unmapping */
            for (size_t j = 0; j < i; j++) {
                uint64_t p = 0;
                if (vmm_virt_to_phys(start + j * PAGE_SIZE, &p) == 0 && p)
                    pmm_free_frame(p);
                vmm_unmap_page(start + j * PAGE_SIZE);
            }
            return 0;
        }
        memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (vmm_map_page(start + i * PAGE_SIZE, phys, vmm_flags) < 0) {
            pmm_free_frame(phys);
            for (size_t j = 0; j < i; j++) {
                uint64_t p = 0;
                if (vmm_virt_to_phys(start + j * PAGE_SIZE, &p) == 0 && p)
                    pmm_free_frame(p);
                vmm_unmap_page(start + j * PAGE_SIZE);
            }
            return 0;
        }
    }
    return start;
}

/* ── vmm_free — Free virtual memory pages ────────────────────── */
static int vmm_free(uint64_t addr, size_t size)
{
    if (addr == 0 || size == 0)
        return -EINVAL;
    if (addr + size < addr)
        return -EOVERFLOW;

    uint64_t start = addr & ~(PAGE_SIZE - 1ULL);
    uint64_t end = ((addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL));

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys = 0;
        if (vmm_virt_to_phys(va, &phys) == 0 && phys)
            pmm_free_frame(phys);
        vmm_unmap_page(va);
    }
    return 0;
}

/* ── vmm_protect — Change page protection ────────────────────── */
static int vmm_protect(uint64_t addr, size_t size, int new_flags)
{
    if (addr == 0 || size == 0)
        return -EINVAL;
    if (addr + size < addr)
        return -EOVERFLOW;

    uint64_t start = addr & ~(PAGE_SIZE - 1ULL);
    uint64_t end = ((addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL));

    uint64_t pte_flags = 0;
    if (new_flags & 1) pte_flags |= VMM_FLAG_PRESENT;  /* PROT_READ */
    if (new_flags & 2) pte_flags |= VMM_FLAG_WRITE;    /* PROT_WRITE */
    if (!(new_flags & 4)) pte_flags |= VMM_FLAG_NOEXEC; /* no exec -> NX */

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t phys = 0;
        if (vmm_virt_to_phys(va, &phys) < 0)
            continue;
        vmm_unmap_page(va);
        vmm_map_page(va, phys, pte_flags);
    }
    return 0;
}

/* ── vmm_sync — Flush data cache for mapped pages ───────────── */
static int vmm_sync(uint64_t addr, size_t size)
{
    (void)addr;
    (void)size;
    /* On x86-64 with write-back cache, no explicit flush needed
     * for coherency (cache is coherent).  For device memory or
     * non-temporal stores, a WBINVD or CLFLUSH would be needed.
     * This stub is a no-op for standard write-back memory. */
    return 0;
}

/* ── vmm_flush_tlb — Flush TLB for a range of pages ─────────── */
static void vmm_flush_tlb(uint64_t addr, size_t size)
{
    if (size == 0) {
        /* Full TLB flush */
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        return;
    }
    if (addr + size < addr) return; /* overflow check */

    uint64_t start = addr & ~(PAGE_SIZE - 1ULL);
    uint64_t end = ((addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1ULL));

    /* Single-page invlpg for each page in the range */
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
}
