#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"
#include "printf.h"
#include "smp.h"
#include "thp.h"

/* NX support status — defined in this file, exported for nx_enforce */
int nx_enabled = 0;

/* If SMP is enabled, use IPI-based TLB shootdown; otherwise local invlpg */
static inline void tlb_flush(uint64_t addr) {
    smp_tlb_shootdown(&addr, 1);
}

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_PCD      (1ULL << 4)
#define PTE_HUGE     (1ULL << 7)
#define PTE_COW      (1ULL << 9)   /* software bit: copy-on-write */
#define PTE_NX       (1ULL << 63)  /* No-Execute (only active when EFER.NXE=1) */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#ifndef FEATURE_NX_SUPPORTED
#define FEATURE_NX_SUPPORTED 1
#endif

/* Check if NX is supported via CPUID */
/* (nx_enabled is defined below and exported for nx_enforce) */

/* Initialize NX support detection */
void vmm_nx_init(void) {
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

int vmm_get_committed(void) { return (int)(vmm_committed_bytes / PAGE_SIZE); }
int vmm_commit(uint64_t bytes) {
    if (vmm_committed_bytes + bytes > VMM_OVERCOMMIT_LIMIT) return -1;
    vmm_committed_bytes += bytes;
    return 0;
}
void vmm_uncommit(uint64_t bytes) {
    if (bytes > vmm_committed_bytes) vmm_committed_bytes = 0;
    else vmm_committed_bytes -= bytes;
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
        if (!frame) return NULL;
        uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt, 0, PAGE_SIZE);
        table[index] = frame | flags | PTE_PRESENT | PTE_WRITE;
        return virt;
    }
    /* If the entry is a 2MB huge page, split it into 512 × 4KB entries. */
    if (table[index] & PTE_HUGE) {
        uint64_t huge = table[index];
        uint64_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) return NULL;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pt_phys);
        uint64_t base  = huge & 0x000FFFFFFFE00000ULL;
        uint64_t pflags = (huge & 0x1FF) & ~(uint64_t)PTE_HUGE;
        for (int i = 0; i < 512; i++)
            pt[i] = (base + (uint64_t)i * PAGE_SIZE) | pflags | PTE_PRESENT;
        table[index] = pt_phys | (flags & 0xFFF) | PTE_PRESENT | PTE_WRITE;
        return pt;
    }
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

void vmm_init(void) {
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

int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table(kernel_pml4, pml4_idx, flags);
    if (!pdpt) return -1;
    uint64_t *pd  = get_or_create_table(pdpt, pdpt_idx, flags);
    if (!pd)  return -1;
    uint64_t *pt  = get_or_create_table(pd, pd_idx, flags);
    if (!pt)  return -1;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    tlb_flush(virt);
    return 0;
}

void vmm_set_range_uncacheable(uint64_t virt, uint64_t size) {
    if (!size) return;
    uint64_t end = virt + size;

    while (virt < end) {
        int pml4_idx = (virt >> 39) & 0x1FF;
        int pdpt_idx = (virt >> 30) & 0x1FF;
        int pd_idx   = (virt >> 21) & 0x1FF;

        if (!(kernel_pml4[pml4_idx] & PTE_PRESENT))
            return;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT))
            return;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

        if (pd[pd_idx] & PTE_HUGE) {
            pd[pd_idx] |= PTE_PCD;
            tlb_flush(virt & ~0x1FFFFFULL);
            virt = (virt & ~0x1FFFFFULL) + (2ULL * 1024 * 1024);
            continue;
        }

        if (!(pd[pd_idx] & PTE_PRESENT))
            return;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);
        int pt_idx = (virt >> 12) & 0x1FF;
        if (pt[pt_idx] & PTE_PRESENT) {
            pt[pt_idx] |= PTE_PCD;
            tlb_flush(virt & ~(PAGE_SIZE - 1ULL));
        }
        virt += PAGE_SIZE;
    }
}

void vmm_unmap_page(uint64_t virt) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) return;
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    pt[pt_idx] = 0;
    tlb_flush(virt);
}

int vmm_virt_to_phys(uint64_t virt, uint64_t *phys) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    /* Check for 2MB huge page */
    if (pd[pd_idx] & (1ULL << 7)) {
        if (phys) *phys = (pd[pd_idx] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
        return 0;
    }

    if (!(pd[pd_idx] & PTE_PRESENT)) return -1;
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    if (!(pt[pt_idx] & PTE_PRESENT)) return -1;
    if (phys) *phys = (pt[pt_idx] & PTE_ADDR_MASK) + (virt & 0xFFF);
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
    uint64_t start = phys & ~(PAGE_SIZE - 1ULL);
    uint64_t end   = (phys + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t off = 0; off < end - start; off += PAGE_SIZE) {
        uint64_t vaddr = KERNEL_VMA_OFFSET + start + off;
        if (vmm_map_page(vaddr, start + off, flags) < 0)
            return NULL;
    }
    return (void *)(KERNEL_VMA_OFFSET + phys);
}

/* Unmap a region previously mapped with vmm_map_phys. */
void vmm_unmap_phys(void *vaddr, uint64_t size) {
    uint64_t start = (uint64_t)(uintptr_t)vaddr & ~(PAGE_SIZE - 1ULL);
    uint64_t end   = ((uint64_t)(uintptr_t)vaddr + size + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE)
        vmm_unmap_page(addr);
}

/* ------------------------------------------------------------------ */
/*  Per-process user address space support                             */
/* ------------------------------------------------------------------ */

uint64_t *vmm_create_user_pml4(void) {
    /* Allocate a new PML4 and copy the upper-half kernel mappings */
    uint64_t frame = pmm_alloc_frame();
    if (!frame) return NULL;
    uint64_t *pml4 = (uint64_t *)PHYS_TO_VIRT(frame);
    memset(pml4, 0, PAGE_SIZE);

    /* Copy kernel half (entries 256-511 map kernel space) */
    for (int i = 256; i < 512; i++)
        pml4[i] = kernel_pml4[i] & ~PTE_USER;

    return pml4;
}

static uint64_t *get_or_create_table_in(uint64_t *table, int index, uint64_t flags) {
    if (!(table[index] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return NULL;
        uint64_t *virt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt, 0, PAGE_SIZE);
        table[index] = frame | flags | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

int vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return -1;
    if (virt >= USER_VADDR_MAX) return -1;

    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table_in(pml4, pml4_idx, flags);
    if (!pdpt) return -1;
    uint64_t *pd = get_or_create_table_in(pdpt, pdpt_idx, flags);
    if (!pd) return -1;
    uint64_t *pt = get_or_create_table_in(pd, pd_idx, flags);
    if (!pt) return -1;

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

    if (!(pml4[pml4_idx] & PTE_PRESENT)) return NULL;
    if (!(pml4[pml4_idx] & PTE_USER)) return NULL;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return NULL;
    if (!(pdpt[pdpt_idx] & PTE_USER)) return NULL;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    if (!(pd[pd_idx] & PTE_PRESENT)) return NULL;
    if (!(pd[pd_idx] & PTE_USER)) return NULL;
    if (pde_out) *pde_out = pd[pd_idx];

    if (pd[pd_idx] & (1ULL << 7)) {
        if (pte_out) *pte_out = pd[pd_idx];
        return NULL;
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

        if (!pt) return 0;
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
    if (!dst) return NULL;

    for (int i = 0; i < 256; i++) {
        if (!(src[i] & PTE_PRESENT)) continue;

        uint64_t *src_pdpt = (uint64_t *)PHYS_TO_VIRT(src[i] & PTE_ADDR_MASK);

        uint64_t dst_pdpt_phys = pmm_alloc_frame();
        if (!dst_pdpt_phys) { vmm_destroy_user_pml4(dst); return NULL; }
        uint64_t *dst_pdpt = (uint64_t *)PHYS_TO_VIRT(dst_pdpt_phys);
        memset(dst_pdpt, 0, PAGE_SIZE);
        dst[i] = dst_pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & PTE_PRESENT)) continue;

            uint64_t *src_pd = (uint64_t *)PHYS_TO_VIRT(src_pdpt[j] & PTE_ADDR_MASK);

            uint64_t dst_pd_phys = pmm_alloc_frame();
            if (!dst_pd_phys) { vmm_destroy_user_pml4(dst); return NULL; }
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
                if (!dst_pt_phys) { vmm_destroy_user_pml4(dst); return NULL; }
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
        if (!new_phys) return 0; /* OOM: can't handle */
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
    return (pt[pt_idx] & PTE_PRESENT) ? 1 : 0;
}

int vmm_map_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages,
                       uint64_t flags) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -1;
    if (virt + num_pages * PAGE_SIZE < virt) return -1; /* overflow */
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -1;

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
        if (!phys) {
            /* Unwind: free already-mapped pages */
            for (size_t j = 0; j < i; j++) {
                uint64_t p;
                vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
                if (vmm_virt_to_phys(virt + j * PAGE_SIZE, &p) == 0 && p && p != vmm_zero_page_frame)
                    pmm_unref_frame(p);
            }
            return -1;
        }
        memset((void *)PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        if (vmm_map_user_page(pml4, cur_virt, phys, flags) < 0) {
            pmm_free_frame(phys);
            for (size_t j = 0; j < i; j++) {
                uint64_t p;
                vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
                if (vmm_virt_to_phys(virt + j * PAGE_SIZE, &p) == 0 && p && p != vmm_zero_page_frame)
                    pmm_unref_frame(p);
            }
            return -1;
        }
    }
    return 0;

unwind:
    for (size_t j = 0; j < i; j++) {
        uint64_t p;
        vmm_unmap_user_page(pml4, virt + j * PAGE_SIZE);
        if (vmm_virt_to_phys(virt + j * PAGE_SIZE, &p) == 0 && p) {
            /* Only unref non-zero-page frames; the zero page lives forever */
            if (p != vmm_zero_page_frame)
                pmm_unref_frame(p);
        }
    }
    return -1;
}

int vmm_unmap_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -1;
    if (virt + num_pages * PAGE_SIZE < virt) return -1;
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -1;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t addr = virt + i * PAGE_SIZE;
        uint64_t phys = 0;
        if (vmm_virt_to_phys(addr, &phys) == 0 && phys && phys != vmm_zero_page_frame) {
            pmm_unref_frame(phys);
        }
        vmm_unmap_user_page(pml4, addr);
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
 * The physical memory is provided as a contiguous 2MB block allocated via
 * pmm_alloc_frames(512).  The virtual address MUST be 2MB-aligned.
 *
 * Page-table walk:
 *   PML4[idx4]  → PDPT (created if absent)
 *   PDPT[idx3]  → PD   (created if absent)
 *   PD[idx2]    ← huge-page PDE with PTE_HUGE + base phys + flags
 *
 * No page table (PT) level is created — the PDE itself references the
 * 2MB physical frame directly.
 */
static int vmm_map_user_hugepage_internal(uint64_t *pml4, uint64_t virt,
                                           uint64_t huge_phys, uint64_t flags) {
    /* Validate alignment constraints */
    if (virt & (HUGE_PAGE_SIZE - 1)) return -1;
    if (huge_phys & (HUGE_PAGE_SIZE - 1)) return -1;
    if (virt >= USER_VADDR_MAX) return -1;
    if (!pml4) return -1;

    int idx4 = (virt >> 39) & 0x1FF;
    int idx3 = (virt >> 30) & 0x1FF;
    int idx2 = (virt >> 21) & 0x1FF;

    /* Ensure PDPT entry exists (allocate if absent) */
    if (!(pml4[idx4] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        uint64_t *virt_pdpt = (uint64_t *)PHYS_TO_VIRT(frame);
        memset(virt_pdpt, 0, PAGE_SIZE);
        pml4[idx4] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[idx4] & PTE_ADDR_MASK);

    /* Ensure PD entry exists (allocate if absent) */
    if (!(pdpt[idx3] & PTE_PRESENT)) {
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
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
     * We strip low 12 bits of flags and use the caller-supplied flags. */
    uint64_t pde = (huge_phys & 0x000FFFFFFFE00000ULL)
                   | (flags & 0x1FF)  /* low 9 flag bits fit below HUGE bit */
                   | PTE_HUGE
                   | PTE_PRESENT;
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
    if (!pml4 || virt >= USER_VADDR_MAX) return -1;
    if (num_4k_pages == 0) return 0;

    uint64_t end = virt + (uint64_t)num_4k_pages * PAGE_SIZE;
    if (end < virt || end > USER_VADDR_MAX) return -1;

    uint64_t cur = virt;

    /* ── Phase 1: Leading partial 2MB chunk (4KB pages) ──────────── */
    uint64_t lead_end = (cur + HUGE_PAGE_SIZE) & ~(HUGE_PAGE_SIZE - 1ULL);
    if (lead_end > end) lead_end = end;

    if (cur < lead_end) {
        size_t lead_pages = (lead_end - cur) / PAGE_SIZE;
        if (vmm_map_user_pages(pml4, cur, lead_pages, flags) < 0)
            return -1;
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
            return -1;
        cur += HUGE_PAGE_SIZE;
    }

    /* ── Phase 3: Trailing partial 2MB chunk (4KB pages) ─────────── */
    if (cur < end) {
        size_t trail_pages = (end - cur) / PAGE_SIZE;
        if (vmm_map_user_pages(pml4, cur, trail_pages, flags) < 0)
            return -1;
    }

    return 0;
}

int vmm_set_user_pages_flags(uint64_t *pml4, uint64_t virt, size_t num_pages,
                             uint64_t new_flags) {
    if (!pml4 || virt >= USER_VADDR_MAX) return -1;
    if (virt + num_pages * PAGE_SIZE < virt) return -1;
    if (virt + num_pages * PAGE_SIZE > USER_VADDR_MAX) return -1;

    for (size_t i = 0; i < num_pages; i++) {
        uint64_t addr = virt + i * PAGE_SIZE;
        int pml4_idx = (addr >> 39) & 0x1FF;
        int pdpt_idx = (addr >> 30) & 0x1FF;
        int pd_idx   = (addr >> 21) & 0x1FF;
        int pt_idx   = (addr >> 12) & 0x1FF;

        if (!(pml4[pml4_idx] & PTE_PRESENT)) return -1;
        uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(pml4[pml4_idx] & PTE_ADDR_MASK);
        if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return -1;
        uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);
        if (!(pd[pd_idx] & PTE_PRESENT)) return -1;

        /* Handle 2MB huge pages: update flags directly in the PDE.
         * The PDE low 9 bits (8:0) contain page flags; bits 9-11 are
         * available for software use (PTE_COW, etc.).  We preserve the
         * physical address and HUGE bit, replacing the flags. */
        if (pd[pd_idx] & (1ULL << 7)) {
            uint64_t pde = pd[pd_idx];
            /* Preserve the physical address base and the HUGE bit */
            uint64_t base = pde & 0x000FFFFFFFE00000ULL;
            uint64_t had_big = pde & PTE_HUGE;
            /* Write new flags and re-apply PRESENT + HUGE */
            pd[pd_idx] = base | (new_flags & 0x1FF)
                         | had_big
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

        if (!(pd[pd_idx] & PTE_PRESENT)) return -1;
        uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

        uint64_t pte = pt[pt_idx];
        if (!(pte & PTE_PRESENT)) return -1;

        /* ── COW-aware flag update ──────────────────────────────────
         * If adding write permission to a COW page, break COW first by
         * allocating a private copy.  Otherwise the shared zero page (or
         * any other COW-shared frame) would become writable in this
         * process's mapping while remaining read-only in the other,
         * violating COW semantics. */
        if ((new_flags & VMM_FLAG_WRITE) && (pte & VMM_FLAG_COW)) {
            uint64_t old_phys = pte & PTE_ADDR_MASK;
            uint64_t new_phys = pmm_alloc_frame();
            if (!new_phys) return -1;
            memcpy((void *)PHYS_TO_VIRT(new_phys),
                   (void *)PHYS_TO_VIRT(old_phys), PAGE_SIZE);
            pmm_unref_frame(old_phys);
            uint64_t new_pte = (pte & (PTE_ADDR_MASK | 0xFFF))
                               & ~(uint64_t)VMM_FLAG_COW;
            new_pte = (new_pte & ~PTE_ADDR_MASK) | new_phys;
            new_pte = (new_pte & ~(uint64_t)0xFFF)
                      | (new_flags & 0xFFF) | PTE_PRESENT;
            pt[pt_idx] = new_pte;
            tlb_flush(addr);
            continue;
        }

        /* Preserve physical address, replace flags.
         * Only set PRESENT if the caller asked for it (PROT_NONE clears it). */
        pt[pt_idx] = (pte & PTE_ADDR_MASK) | (new_flags & 0xFFF)
                     | ((new_flags & VMM_FLAG_PRESENT) ? PTE_PRESENT : 0);
        tlb_flush(addr);
    }
    return 0;
}
