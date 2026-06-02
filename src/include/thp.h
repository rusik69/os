#ifndef THP_H
#define THP_H

#include "types.h"

/* THP — Transparent Huge Pages tracking */

/* Huge page sizes */
#define THP_HPAGE_SIZE    (2 * 1024 * 1024)   /* 2 MB */
#define THP_HPAGE_PFN_MASK (~((2 * 1024 * 1024 / 4096) - 1))

/* THP state */
#define THP_RAW           0  /* Raw huge page (not yet split) */
#define THP_SPLIT         1  /* Split into 4K pages */
#define THP_PARTIAL       2  /* Partially split */

/* Initialize THP tracking subsystem */
void thp_init(void);

/* Enable/disable THP */
void thp_set_enabled(int enabled);
int thp_is_enabled(void);

/* Track a huge page mapping */
int thp_track_hugepage(uint64_t virt_addr, uint64_t phys_addr);

/* Untrack a huge page */
void thp_untrack_hugepage(uint64_t virt_addr);

/* Split a transparent huge page (returns number of 4K pages) */
int thp_split_hugepage(uint64_t virt_addr);

/* Get THP statistics */
uint64_t thp_get_total_pages(void);
uint64_t thp_get_merged_pages(void);
uint64_t thp_get_split_pages(void);

/* ── khugepaged daemon — background 4K→2MB promotion ───────────────── */

/* Start the khugepaged daemon (called once during boot) */
void khugepaged_start(void);

/* Enable/disable the khugepaged daemon at runtime */
void khugepaged_set_enabled(int enabled);
int  khugepaged_is_enabled(void);

/* Set the sleep interval between scan cycles (in milliseconds) */
void khugepaged_set_sleep_ms(int ms);
int  khugepaged_get_sleep_ms(void);

/* Get khugepaged statistics */
uint64_t khugepaged_get_scanned(void);
uint64_t khugepaged_get_promoted(void);
uint64_t khugepaged_get_failed(void);

#endif /* THP_H */
