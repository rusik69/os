#ifndef PMM_H
#define PMM_H

#include "types.h"

void pmm_init(uint64_t multiboot_info_phys);
void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size); /* mark a range used */
void pmm_advance_hint(uint64_t phys_addr); /* advance alloc hint past given phys addr */
uint64_t pmm_alloc_frame(void);
uint64_t *pmm_alloc_frames(size_t count);
void pmm_free_frame(uint64_t frame);
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);
/* Reference counting for COW */
void pmm_ref_frame(uint64_t phys);      /* increment refcount */
int  pmm_unref_frame(uint64_t phys);    /* decrement; frees if 0, returns new count */
int  pmm_refcount(uint64_t phys);       /* query current refcount */

/* Page poisoning: fill freed pages with 0xDC and allocated pages with 0xDEADBEEF */
extern int pmm_poison_enabled;
void pmm_set_poison(int enable);

#endif
