#ifndef VMM_H
#define VMM_H

#include "types.h"

#define VMM_FLAG_PRESENT  (1ULL << 0)
#define VMM_FLAG_WRITE    (1ULL << 1)
#define VMM_FLAG_USER     (1ULL << 2)
/* Software bit 9 (available to OS) used for Copy-on-Write */
#define VMM_FLAG_COW      (1ULL << 9)
/* Software bit 10 (available to OS) used for lazy/demand allocation.
 * Pages mapped with this flag share the global zero page; on first write
 * the COW handler allocates a real physical page. */
#define VMM_FLAG_LAZY     (1ULL << 10)
/* Page-level cache disable (PAT bit) for MMIO */
#define VMM_FLAG_NOCACHE  (1ULL << 4)  /* PCD = Page Cache Disable */
#define VMM_FLAG_NOEXEC   (1ULL << 63) /* No-Execute (NX bit) */

/* mmap/mprotect protection flags */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* User virtual address space limit */
#define USER_VADDR_MAX 0x0000800000000000ULL

void vmm_init(void);
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_set_range_uncacheable(uint64_t virt, uint64_t size);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_physaddr(uint64_t virt);
int vmm_virt_to_phys(uint64_t virt, uint64_t *phys);
uint64_t *vmm_get_pml4(void);

/* Map/unmap physical memory in the kernel's high-half VMA space. */
void *vmm_map_phys(uint64_t phys, uint64_t size, uint64_t flags);
void  vmm_unmap_phys(void *vaddr, uint64_t size);

/* Per-process user address space */
uint64_t *vmm_create_user_pml4(void);
uint64_t *vmm_clone_user_pml4(uint64_t *src); /* deep-copy user half */
int vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_user_page(uint64_t *pml4, uint64_t virt);
void vmm_switch_pml4(uint64_t *pml4);
void vmm_destroy_user_pml4(uint64_t *pml4);

/* User-memory validation helpers for syscall boundary checks */
int vmm_user_range_ok(uint64_t *pml4, uint64_t addr, uint64_t len, int write);
int vmm_user_string_ok(uint64_t *pml4, uint64_t addr, uint64_t max_len);

/* COW fault handler: handles write fault on a COW page.
 * Returns 1 if fault was handled, 0 if not a COW fault. */
int vmm_handle_cow_fault(uint64_t *pml4, uint64_t virt);

/* mmap/munmap/mprotect helpers */
int vmm_map_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages, uint64_t flags);
int vmm_unmap_user_pages(uint64_t *pml4, uint64_t virt, size_t num_pages);
int vmm_set_user_pages_flags(uint64_t *pml4, uint64_t virt, size_t num_pages, uint64_t new_flags);
int vmm_page_is_mapped_user(uint64_t *pml4, uint64_t virt);

/* Huge page (2MB) support for anonymous mappings */
#define HUGE_PAGE_SIZE      (2ULL * 1024 * 1024)
#define HUGE_PAGE_NFRAMES   512
int vmm_map_user_huge_pages(uint64_t *pml4, uint64_t virt, size_t num_4k_pages, uint64_t flags);

/* NX bit support */
void vmm_nx_init(void);
int vmm_check_nx(uint64_t *pml4, uint64_t virt, int write, int exec);

/* VM statistics counters (for /proc/vmstat) */
extern uint64_t vm_pgalloc;
extern uint64_t vm_pgfree;
extern uint64_t vm_pgfault;
extern uint64_t vm_pgmajfault;
extern uint64_t vm_pgswapin;
extern uint64_t vm_pgswapout;
extern uint64_t vm_pgin;
extern uint64_t vm_pgout;
extern uint64_t vm_hugepages;       /* number of 2MB huge pages allocated */

/* Shared zero page for demand/lazy allocation.
 * A single zero-filled physical frame shared among all lazy mappings.
 * Incremented on each lazy map; never freed. */
extern uint64_t vmm_zero_page_frame;

/* Memory overcommit tracking */
extern uint64_t vmm_committed_bytes;
#define VMM_OVERCOMMIT_LIMIT (256ULL * 1024 * 1024) /* 256 MB overcommit limit */
int vmm_get_committed(void);
int vmm_commit(uint64_t bytes);
void vmm_uncommit(uint64_t bytes);

#endif
