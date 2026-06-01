#ifndef KSM_H
#define KSM_H

#include "types.h"

/* KSM — Kernel Same-page Merging */

/* Initialize KSM subsystem */
void ksm_init(void);

/* Enable/disable KSM scanning */
void ksm_set_enabled(int enabled);
int ksm_is_enabled(void);

/* Manually register a memory region for KSM scanning */
int ksm_register_region(uint64_t addr, size_t size);

/* Unregister a KSM region */
int ksm_unregister_region(uint64_t addr);

/* Trigger a scan cycle (merge eligible pages) */
void ksm_scan_cycle(void);

/* Get KSM statistics */
uint64_t ksm_get_merged_pages(void);
uint64_t ksm_get_unmergeable_pages(void);
uint64_t ksm_get_scan_count(void);

#endif /* KSM_H */
