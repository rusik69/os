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

#endif /* PAGE_IDLE_H */
