#ifndef IOMMU_H
#define IOMMU_H

#include "types.h"
#include "pmm.h"
#include "heap.h"
#include "string.h"

/*
 * IOMMU framework stub with real page table operations
 * for future AMD-Vi (IOMMU) and Intel VT-d support.
 *
 * IOMMU page tables are 4-level (like x86-64 host page tables):
 *   Level 0: 4 KB pages (12-bit offset within page)
 *   Level 1-3: Table entries pointing to next level
 *
 * Each entry is 8 bytes (64-bit).
 * Level 3 table has 512 entries (covering 48-bit address space).
 *
 * For IOMMU we use a simplified flat mapping with 2MB or 4KB granularity.
 * Future: Support for AMD-Vi device table entries and Intel VT-d
 * context/root entry formats.
 */

/* ── Page table constants ──────────────────────────────────────────── */

#define IOMMU_PAGE_SIZE      4096
#define IOMMU_LARGE_PAGE     (2ULL * 1024 * 1024)  /* 2 MB */
#define IOMMU_PAGE_SHIFT     12
#define IOMMU_LARGE_SHIFT    21
#define IOMMU_PT_ENTRIES     512
#define IOMMU_PT_LEVELS      4

/* Page table entry bits */
#define IOMMU_PTE_PRESENT    (1ULL << 0)
#define IOMMU_PTE_RW         (1ULL << 1)
#define IOMMU_PTE_USER       (1ULL << 2)  /* Allow user access */
#define IOMMU_PTE_NX         (1ULL << 63) /* No-execute */

/* ── IOMMU domain ──────────────────────────────────────────────────── */

struct iommu_domain {
    uint64_t *pgd;            /* Pointer to level-3 page directory (4KB table) */
    int       pgd_phys;       /* Physical address of PGD (for hardware) */
    uint64_t  flags;          /* Domain flags */
    uint64_t  addr_range;     /* Max IOVA address (48-bit = 256 TB) */
    int       initialized;
};

/* IOMMU access flags (for iommu_map) */
#define IOMMU_READ   (1UL << 0)
#define IOMMU_WRITE  (1UL << 1)
#define IOMMU_EXEC   (1UL << 2)

/* ── Internal helpers ──────────────────────────────────────────────── */

/* Extract a page table index from a virtual address for a given level.
 * Level 3 (highest): bits 39-47
 * Level 2: bits 30-38
 * Level 1: bits 21-29
 * Level 0: bits 12-20
 */
static inline int iommu_pte_index(uint64_t iova, int level) {
    int shift = IOMMU_PAGE_SHIFT + level * 9;
    return (int)((iova >> shift) & 0x1FF);
}

/* Allocate a zeroed page for a page table */
static inline uint64_t *iommu_alloc_ptable(void) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return NULL;
    uint64_t *virt = (uint64_t *)(phys + 0xFFFF800000000000ULL);
    memset(virt, 0, IOMMU_PAGE_SIZE);
    return virt;
}

/* Free a page table page */
static inline void iommu_free_ptable(uint64_t *table) {
    uint64_t phys = (uint64_t)table - 0xFFFF800000000000ULL;
    pmm_free_frame(phys);
}

/* Install a page table entry at the given level with a link to the next level */
static inline void iommu_install_table(uint64_t *table, int index, uint64_t next_phys, uint64_t flags) {
    uint64_t entry = next_phys | IOMMU_PTE_PRESENT | IOMMU_PTE_RW | (flags & (IOMMU_PTE_USER | IOMMU_PTE_NX));
    table[index] = entry;
}

/* Walk the page table for a given IOVA, creating intermediate tables as needed */
static inline uint64_t *iommu_walk(uint64_t *pgd, uint64_t iova, int create) {
    uint64_t *table = pgd;

    for (int level = 3; level >= 0; level--) {
        int idx = iommu_pte_index(iova, level);
        uint64_t entry = table[idx];

        if (level == 0) {
            /* Leaf level — return the entry's physical page address or PTE */
            return &table[idx];
        }

        if (!(entry & IOMMU_PTE_PRESENT)) {
            if (!create) return NULL;
            /* Allocate next-level table */
            uint64_t *next = iommu_alloc_ptable();
            if (!next) return NULL;
            uint64_t next_phys = (uint64_t)next - 0xFFFF800000000000ULL;
            iommu_install_table(table, idx, next_phys, 0);
            entry = table[idx];
        }

        /* Walk to next level */
        uint64_t next_virt = (entry & ~0xFFFULL) + 0xFFFF800000000000ULL;
        table = (uint64_t *)next_virt;
    }

    return NULL;
}

/* ── Public API ─────────────────────────────────────────────────────── */

/* Allocate an IOMMU domain with real page table allocation */
static inline struct iommu_domain *iommu_domain_alloc(void) {
    struct iommu_domain *domain = (struct iommu_domain *)kmalloc(sizeof(struct iommu_domain));
    if (!domain) return NULL;

    domain->pgd = iommu_alloc_ptable();
    if (!domain->pgd) {
        kfree(domain);
        return NULL;
    }

    domain->pgd_phys = 0;
    domain->flags = 0;
    domain->addr_range = 0xFFFFFFFFFFFFULL;  /* 48-bit address space */
    domain->initialized = 1;

    return domain;
}

/* Map a physical region into an IOMMU domain.
 * Creates page table entries for the IOVA range.
 * Returns 0 on success, -1 on failure. */
static inline int iommu_map(struct iommu_domain *domain,
                             uint64_t iova, uint64_t phys_addr,
                             size_t size, uint64_t flags) {
    if (!domain || !domain->initialized || !domain->pgd) return -1;
    if (size == 0) return -1;

    uint64_t prot = 0;
    if (flags & IOMMU_READ)  prot |= IOMMU_PTE_PRESENT;
    if (flags & IOMMU_WRITE) prot |= IOMMU_PTE_RW;
    /* No-execute by default unless IOMMU_EXEC is set */
    if (!(flags & IOMMU_EXEC)) prot |= IOMMU_PTE_NX;

    uint64_t offset = 0;
    while (offset < size) {
        uint64_t iova_addr = iova + offset;
        uint64_t phys_addr_page = phys_addr + offset;

        /* Walk page table, creating intermediate entries */
        uint64_t *pte = iommu_walk(domain->pgd, iova_addr, 1);
        if (!pte) return -1;

        /* Set the page table entry to point to the physical page */
        *pte = (phys_addr_page & ~0xFFFULL) | prot | IOMMU_PTE_PRESENT | IOMMU_PTE_RW;

        offset += IOMMU_PAGE_SIZE;
    }

    return 0;
}

/* Unmap a region from an IOMMU domain.
 * Clears page table entries for the IOVA range.
 * Returns 0 on success. */
static inline int iommu_unmap(struct iommu_domain *domain,
                               uint64_t iova, size_t size) {
    if (!domain || !domain->initialized || !domain->pgd) return -1;

    uint64_t offset = 0;
    while (offset < size) {
        uint64_t iova_addr = iova + offset;

        uint64_t *pte = iommu_walk(domain->pgd, iova_addr, 0);
        if (pte) {
            *pte = 0;  /* Clear the entry */
        }

        offset += IOMMU_PAGE_SIZE;
    }

    return 0;
}

/* Free an IOMMU domain, releasing all page table pages.
 * Recursively walks and frees all page table levels. */
static inline void iommu_domain_free(struct iommu_domain *domain) {
    if (!domain || !domain->pgd) return;

    /* Walk all entries and free sub-tables */
    for (int i3 = 0; i3 < IOMMU_PT_ENTRIES; i3++) {
        uint64_t e3 = domain->pgd[i3];
        if (!(e3 & IOMMU_PTE_PRESENT)) continue;

        uint64_t *t3 = (uint64_t *)((e3 & ~0xFFFULL) + 0xFFFF800000000000ULL);
        for (int i2 = 0; i2 < IOMMU_PT_ENTRIES; i2++) {
            uint64_t e2 = t3[i2];
            if (!(e2 & IOMMU_PTE_PRESENT)) continue;

            uint64_t *t2 = (uint64_t *)((e2 & ~0xFFFULL) + 0xFFFF800000000000ULL);
            for (int i1 = 0; i1 < IOMMU_PT_ENTRIES; i1++) {
                uint64_t e1 = t2[i1];
                if (!(e1 & IOMMU_PTE_PRESENT)) continue;

                /* At level 1, the entry points to a level-0 page table */
                uint64_t *t1 = (uint64_t *)((e1 & ~0xFFFULL) + 0xFFFF800000000000ULL);
                iommu_free_ptable(t1);
            }
            iommu_free_ptable(t2);
        }
        iommu_free_ptable(t3);
    }

    iommu_free_ptable(domain->pgd);
    kfree(domain);
}

#endif /* IOMMU_H */
