#include "vmm.h"
#include "pmm.h"
#include "string.h"
#include "types.h"

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

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
