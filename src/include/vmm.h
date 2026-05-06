#ifndef VMM_H
#define VMM_H

#include "types.h"

#define VMM_FLAG_PRESENT  (1 << 0)
#define VMM_FLAG_WRITE    (1 << 1)
#define VMM_FLAG_USER     (1 << 2)

void vmm_init(void);
void vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap_page(uint64_t virt);
uint64_t vmm_get_physaddr(uint64_t virt);
uint64_t *vmm_get_pml4(void);

/* Per-process user address space */
uint64_t *vmm_create_user_pml4(void);
void vmm_map_user_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_switch_pml4(uint64_t *pml4);
void vmm_destroy_user_pml4(uint64_t *pml4);

/* User-memory validation helpers for syscall boundary checks */
int vmm_user_range_ok(uint64_t *pml4, uint64_t addr, uint64_t len, int write);
int vmm_user_string_ok(uint64_t *pml4, uint64_t addr, uint64_t max_len);

#endif
