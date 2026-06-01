#ifndef PAGE_OWNER_H
#define PAGE_OWNER_H

#include "types.h"

/* Page owner tracking: record which PID allocated each physical page */
#define PAGE_OWNER_TRACKING

void page_owner_set(uint64_t phys_addr, uint32_t pid);
uint32_t page_owner_get(uint64_t phys_addr);
void page_owner_clear(uint64_t phys_addr);
void page_owner_init(void);
void page_owner_dump(void);

#endif /* PAGE_OWNER_H */
