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

#endif
