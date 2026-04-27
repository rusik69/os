#ifndef PMM_H
#define PMM_H

#include "types.h"

void pmm_init(uint64_t multiboot_info_phys);
void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size); /* mark a range used */
uint64_t pmm_alloc_frame(void);
void pmm_free_frame(uint64_t frame);
uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);

#endif
