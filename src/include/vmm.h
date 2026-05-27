#ifndef VMM_H
#define VMM_H

#include "types.h"

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITE    (1 << 1)
#define VMM_FLAG_USER     (1 << 2)
/* Software bit 9 (available to OS) used for Copy-on-Write */
#define VMM_FLAG_COW      (1ULL << 9)

void vmm_init(void);
int vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_set_range_uncacheable(uint64_t virt, uint64_t size);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_physaddr(uint64_t virt);
uint64_t *vmm_get_pml4(void);

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

#endif
