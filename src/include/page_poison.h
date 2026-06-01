#ifndef PAGE_POISON_H
#define PAGE_POISON_H

#include "types.h"

/* Page poisoning: fill freed pages with 0xDC to catch use-after-free */

/* Initialize page poisoning subsystem */
void page_poison_init(void);

/* Set poison value for freed pages */
void page_poison_set_freed_value(uint8_t val);

/* Get current poison value */
uint8_t page_poison_get_freed_value(void);

/* Check if page poisoning is active */
int page_poison_is_active(void);

/* Manually poison a memory region */
void poison_region(void *addr, size_t size);

/* Check a region for poison corruption (returns 0 if clean) */
int poison_check_region(const void *addr, size_t size, uint8_t poison_val);

#endif /* PAGE_POISON_H */
