#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_COW      (1ULL << 9)   /* software bit: copy-on-write */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL
#define USER_VADDR_MAX 0x0000800000000000ULL

static uint64_t *kernel_pml4;

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
    }
    return (uint64_t *)PHYS_TO_VIRT(table[index] & PTE_ADDR_MASK);
}

void vmm_init(void) {
    /* Use current PML4 set up by boot code */
    kernel_pml4 = (uint64_t *)PHYS_TO_VIRT(read_cr3() & PTE_ADDR_MASK);
}

void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table(kernel_pml4, pml4_idx, flags);
    if (!pdpt) return;
    uint64_t *pd = get_or_create_table(pdpt, pdpt_idx, flags);
    if (!pd) return;
    uint64_t *pt = get_or_create_table(pd, pd_idx, flags);
    if (!pt) return;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
    invlpg(virt);
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
    invlpg(virt);
}

uint64_t vmm_get_physaddr(uint64_t virt) {
    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    if (!(kernel_pml4[pml4_idx] & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)PHYS_TO_VIRT(kernel_pml4[pml4_idx] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_idx] & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)PHYS_TO_VIRT(pdpt[pdpt_idx] & PTE_ADDR_MASK);

    /* Check for 2MB huge page */
    if (pd[pd_idx] & (1ULL << 7)) {
        return (pd[pd_idx] & 0x000FFFFFFFE00000ULL) + (virt & 0x1FFFFF);
    }

    if (!(pd[pd_idx] & PTE_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[pd_idx] & PTE_ADDR_MASK);

    if (!(pt[pt_idx] & PTE_PRESENT)) return 0;
    return (pt[pt_idx] & PTE_ADDR_MASK) + (virt & 0xFFF);
}

uint64_t *vmm_get_pml4(void) {
    return kernel_pml4;
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

void vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    if (!pml4) return;
    if (virt >= USER_VADDR_MAX) return;

    int pml4_idx = (virt >> 39) & 0x1FF;
    int pdpt_idx = (virt >> 30) & 0x1FF;
    int pd_idx   = (virt >> 21) & 0x1FF;
    int pt_idx   = (virt >> 12) & 0x1FF;

    uint64_t *pdpt = get_or_create_table_in(pml4, pml4_idx, flags);
    if (!pdpt) return;
    uint64_t *pd = get_or_create_table_in(pdpt, pdpt_idx, flags);
    if (!pd) return;
    uint64_t *pt = get_or_create_table_in(pd, pd_idx, flags);
    if (!pt) return;

    pt[pt_idx] = (phys & PTE_ADDR_MASK) | (flags & 0xFFF) | PTE_PRESENT;
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
            if (write && !(pde & PTE_WRITE)) return 0;
            cur = (cur & ~0x1FFFFFULL) + 0x200000ULL;
            continue;
        }

        if (!pt) return 0;
        if (!(pte & PTE_PRESENT) || !(pte & PTE_USER)) return 0;
        if (write && !(pte & PTE_WRITE)) return 0;

        cur = (cur & ~0xFFFULL) + 0x1000ULL;
    }

    return 1;
}

int vmm_user_string_ok(uint64_t *pml4, uint64_t addr, uint64_t max_len) {
    if (!pml4 || addr >= USER_VADDR_MAX || max_len == 0) return 0;
    for (uint64_t i = 0; i < max_len; i++) {
        uint64_t cur = addr + i;
        if (cur < addr || cur >= USER_VADDR_MAX) return 0;
        if (!vmm_user_range_ok(pml4, cur, 1, 0)) return 0;
        if (*(const char *)cur == '\0') return 1;
    }
    return 0;
}

void vmm_switch_pml4(uint64_t *pml4) {
    /* Convert virtual address of PML4 to physical for CR3 */
    uint64_t phys = (uint64_t)pml4; /* identity mapped */
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
        if (!dst_pdpt_phys) return dst;
        uint64_t *dst_pdpt = (uint64_t *)PHYS_TO_VIRT(dst_pdpt_phys);
        memset(dst_pdpt, 0, PAGE_SIZE);
        dst[i] = dst_pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & PTE_PRESENT)) continue;

            uint64_t *src_pd = (uint64_t *)PHYS_TO_VIRT(src_pdpt[j] & PTE_ADDR_MASK);

            uint64_t dst_pd_phys = pmm_alloc_frame();
            if (!dst_pd_phys) return dst;
            uint64_t *dst_pd = (uint64_t *)PHYS_TO_VIRT(dst_pd_phys);
            memset(dst_pd, 0, PAGE_SIZE);
            dst_pdpt[j] = dst_pd_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & PTE_PRESENT)) continue;
                /* Pass 2MB huge pages through unchanged */
                if (src_pd[k] & (1ULL << 7)) { dst_pd[k] = src_pd[k]; continue; }

                uint64_t *src_pt = (uint64_t *)PHYS_TO_VIRT(src_pd[k] & PTE_ADDR_MASK);

                uint64_t dst_pt_phys = pmm_alloc_frame();
                if (!dst_pt_phys) return dst;
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
                if (pd[k] & (1ULL << 7)) continue; /* skip 2MB pages */
                uint64_t *pt = (uint64_t *)PHYS_TO_VIRT(pd[k] & PTE_ADDR_MASK);
                /* Free each mapped leaf page frame via refcount */
                for (int l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT)
                        pmm_unref_frame(pt[l] & PTE_ADDR_MASK);
                }
                pmm_free_frame(pd[k] & PTE_ADDR_MASK); /* free PT */
            }
            pmm_free_frame(pdpt[j] & PTE_ADDR_MASK); /* free PD */
        }
        pmm_free_frame(pml4[i] & PTE_ADDR_MASK); /* free PDPT */
    }
    /* Free the PML4 itself */
    uint64_t pml4_phys = (uint64_t)pml4; /* identity mapped */
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
    invlpg(virt);
    return 1;
}
