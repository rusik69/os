#ifndef PAGE_IDLE_H
#define PAGE_IDLE_H

#include "types.h"

/* Mark a physical page as recently accessed (set the accessed bit). */
int page_idle_mark_accessed(uint64_t phys);

/* Return non-zero if the physical page appears idle (accessed bit clear). */
int page_idle_is_idle(uint64_t phys);

/* Clear the idle / accessed flag on a physical page. */
int page_idle_clear(uint64_t phys);

/* Walk all available pages and report which ones are idle.
 * Returns the number of idle pages found. */
int page_idle_scan_idle(void);

/* Initialise the page idle subsystem. */
void page_idle_init(void);

/*
 * Read a portion of the idle/accessed bitmap for physical pages.
 * bitmap must point to a buffer of nr_pages bytes. For each page index i,
 * bit (1<<(i&7)) at byte offset i/7: 1 = idle, 0 = accessed.
 * Returns 0 on success, negative on error.
 */
int page_idle_bitmap_read(uint64_t start_pfn, uint64_t nr_pfns, uint8_t *bitmap);

/*
 * Write a portion of the idle/accessed bitmap.
 * For each page whose corresponding bit in bitmap is 1, clear the
 * accessed bit (mark as idle).  For bit = 0, do nothing (preserve accessed).
 * Returns 0 on success, negative on error.
 */
int page_idle_bitmap_write(uint64_t start_pfn, uint64_t nr_pfns, const uint8_t *bitmap);

#endif /* PAGE_IDLE_H */
