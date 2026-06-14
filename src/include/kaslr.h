#ifndef KASLR_H
#define KASLR_H

#include "types.h"

/*
 * KASLR — Kernel Address Space Layout Randomization
 *
 * Randomizes the kernel base virtual address at boot time to make
 * kernel memory layout unpredictable, mitigating exploits that rely
 * on knowing the location of kernel code/data.
 *
 * The randomization offset is chosen once during early boot (before
 * paging is fully set up) and remains fixed for the entire session.
 * The offset is 2MB-aligned and in the range [0, KASLR_MAX_OFFSET].
 *
 * Disable via "nokaslr" on the kernel command line (uses existing
 * aslr_disabled flag from aslr.c).
 */

/* Maximum randomization offset: 512 MB */
#define KASLR_MAX_OFFSET    (512ULL * 1024 * 1024)

/* Alignment: 2 MB (huge page size) */
#define KASLR_ALIGN         (2ULL * 1024 * 1024)

/*
 * The chosen kernel ASLR offset.
 * 0 means KASLR is disabled or not yet initialized.
 * This is the number of bytes added to the kernel's high-half VMA base
 * (KERNEL_VMA_OFFSET) to produce the effective kernel virtual address base.
 */
extern uint64_t kaslr_offset;

/* ── Initialisation ──────────────────────────────────────────────── */

/**
 * kaslr_get_offset - Generate and return the KASLR offset.
 *
 * On first call, generates a random 2MB-aligned offset in the range
 * [0, KASLR_MAX_OFFSET] using RDRAND (if available) or TSC+RTC fallback.
 * The result is cached in the global kaslr_offset and returned.
 *
 * Subsequent calls return the cached value.
 *
 * Can be called from early boot (before heap, before printf is fully
 * initialized) as long as RDRAND/TSC instructions are available.
 *
 * Returns: The KASLR offset in bytes (always 2MB-aligned).
 */
uint64_t kaslr_get_offset(void);

/**
 * kaslr_init - Initialize KASLR subsystem.
 *
 * Calls kaslr_get_offset() to seed the offset, then logs the result.
 * Also checks the "nokaslr" kernel command-line parameter (via
 * aslr_disabled).  If ASLR is disabled globally, the offset is forced to 0.
 *
 * Should be called as early as possible in kernel_main(), after
 * serial/VGA is initialized for logging, but before any sensitive
 * kernel address is exposed.
 */
void kaslr_init(void);

/* ── Query ───────────────────────────────────────────────────────── */

/**
 * kaslr_is_active - Returns 1 if KASLR offset is non-zero (active).
 */
static inline int kaslr_is_active(void) {
    extern uint64_t kaslr_offset;
    return kaslr_offset != 0;
}

/**
 * kaslr_apply_offset - Add the KASLR offset to a base virtual address.
 * Used by subsystems that need to map themselves at the randomized base.
 */
static inline uint64_t kaslr_apply_offset(uint64_t vaddr) {
    extern uint64_t kaslr_offset;
    return vaddr + kaslr_offset;
}

#endif /* KASLR_H */
