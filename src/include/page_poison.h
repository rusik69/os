#ifndef PAGE_POISON_H
#define PAGE_POISON_H

#include "types.h"

/*
 * Page poisoning — init-stage-aware memory poisoning (Item 127).
 *
 * Supports two init stages:
 *   MEMINIT_EARLY — before the slab/page allocators are fully online.
 *                   Uses poison value 0x6b to catch early-boot use-after-free.
 *   MEMINIT_LATE  — after all kernel subsystems are initialized.
 *                   Uses poison value 0xdc (standard freed-page pattern).
 *
 * On transition from EARLY to LATE, all previously poisoned pages are
 * verified to still be clean, catching any use-before-init bugs.
 */

/* ── Init stage tracking ─────────────────────────────────────────── */

enum meminit_stage {
    MEMINIT_NONE   = 0,    /* Before page_poison_init() called */
    MEMINIT_EARLY  = 1,    /* Early boot — conservative poisoning */
    MEMINIT_LATE   = 2,    /* Full system — standard poisoning */
};

/* Get the current init stage */
enum meminit_stage page_poison_get_stage(void);

/* Transition to MEMINIT_LATE.
 * On transition, verifies all EARLY-poisoned pages are still intact.
 * Logs a warning if any corruption is detected.
 * Returns 0 on success, negative on corruption detection. */
int page_poison_enter_late_stage(void);

/* ── Core poisoning API ──────────────────────────────────────────── */

/* Initialize page poisoning subsystem (starts in MEMINIT_EARLY) */
void page_poison_init(void);

/* Currently active poison value (depends on init stage) */
uint8_t page_poison_get_freed_value(void);

/* Override poison value for the current stage */
void page_poison_set_freed_value(uint8_t val);

/* Check if page poisoning is active */
int page_poison_is_active(void);

/* Poison values per stage */
#define POISON_EARLY_FREED  0x6B   /* MEMINIT_EARLY freed-page pattern */
#define POISON_LATE_FREED   0xDC   /* MEMINIT_LATE  freed-page pattern */
#define POISON_USE_BEFORE_INIT 0x5A /* Detected on use-before-init */
#define POISON_REDZONE      0xEA   /* Redzone filler (for slab redzoning) */

/* Manually poison a memory region using the current stage's pattern */
void poison_region(void *addr, size_t size);

/* Check a region for poison corruption (returns 0 if clean, >0 if corrupted) */
int poison_check_region(const void *addr, size_t size, uint8_t poison_val);

/* Poison with a specific value (ignores current stage) */
void poison_region_with_value(void *addr, size_t size, uint8_t val);

#endif /* PAGE_POISON_H */
