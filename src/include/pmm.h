#ifndef PMM_H
#define PMM_H

#include "types.h"

void pmm_init(uint64_t multiboot_info_phys);
void pmm_reserve_frames(uint64_t phys_start, uint64_t byte_size); /* mark a range used */
void pmm_advance_hint(uint64_t phys_addr); /* advance alloc hint past given phys addr */
uint64_t pmm_alloc_frame(void);
uint64_t *pmm_alloc_frames(size_t count);
void pmm_free_frame(uint64_t frame);
void pmm_free_frames_contiguous(uint64_t phys, size_t count); /* free contiguous frames */

/* Migration-type aware allocation (Item 121):
 * pmm_alloc_frame_migrate() prefers pageblocks of the given migration type
 * to reduce fragmentation.  Falls back to other types if needed.
 * pmm_alloc_frame() remains as MIGRATE_MOVABLE for backward compat. */
#include "pageblock.h"
uint64_t pmm_alloc_frame_migrate(enum migratetype mt);

uint64_t pmm_get_total_frames(void);
uint64_t pmm_get_used_frames(void);
/* Reference counting for COW */
void pmm_ref_frame(uint64_t phys);      /* increment refcount */
int  pmm_unref_frame(uint64_t phys);    /* decrement; frees if 0, returns new count */
int  pmm_refcount(uint64_t phys);       /* query current refcount */

/* Dump detailed physical memory statistics (total, used, free, largest block, fragmentation) */
void pmm_dump_stats(void);

/* Return the size (in frames) of the largest contiguous free block */
uint64_t pmm_largest_free_block(void);

/* Return the number of distinct free page runs (higher = more fragmented) */
uint64_t pmm_free_block_count(void);

/* Scan the bitmap for the first free region starting at or after start_frame.
 * Returns the starting frame number, or ~0ULL if no free region remains.
 * On success, *out_count is set to the number of contiguous free frames. */
uint64_t pmm_find_free_region(uint64_t start_frame, uint64_t *out_count);

/* Page poisoning: fill freed pages with 0xDC and allocated pages with 0xDEADBEEF */
extern int pmm_poison_enabled;
void pmm_set_poison(int enable);

/* Memory reclaim watermark — minimum free pages before reclaim triggers */
uint64_t pmm_get_reclaim_watermark(void);
void pmm_set_reclaim_watermark(uint64_t pages);

/* Check if memory is below reclaim watermark */
int pmm_below_watermark(void);

#endif
