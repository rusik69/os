#ifndef ASLR_H
#define ASLR_H

#include "types.h"

/*
 * ASLR (Address Space Layout Randomization) — user-space AND kernel-space.
 *
 * Provides:
 *   - Random stack base offset per process (up to ASLR_STACK_RANDOM_BYTES)
 *   - Random mmap base for anonymous mappings
 *   - Random brk/ELF base offset
 *   - Random kernel module base offset (KASLR for modules)
 *   - Random kernel stack offset per CPU
 *
 * All features can be disabled globally with the "nokaslr" kernel cmdline.
 * The entropy is derived from the kernel's prng_rand64() which is seeded
 * from RTC + tick count + RDRAND (if available) at boot.
 */

/* Runtime ASLR toggle — set by aslr_init() based on cmdline "nokaslr" */
extern int aslr_disabled;

/* Maximum random shift for user stack top (in pages) */
#define ASLR_STACK_RANDOM_PAGES  16   /* up to 64KB random offset */

/* Maximum random shift for mmap base (in pages) */
#define ASLR_MMAP_RANDOM_PAGES   256  /* up to 1MB random offset */

/* Maximum random shift for brk/data segment (in pages) */
#define ASLR_BRK_RANDOM_PAGES    32

/* Maximum random shift for kernel module base (in pages).
 * Modules are loaded in a 64MB region (MODULES_VADDR..MODULES_VADDR+64MB).
 * Shifting by up to 8192 pages (32 MB) randomizes the base while retaining
 * at least 32 MB of usable space. */
#define ASLR_MODULE_RANDOM_PAGES 8192

/* Maximum random shift for per-CPU kernel stack (in pages).
 * Kernel stacks are large (16KB typical), so up to 8 pages = 32KB offset
 * keeps them within the same 64KB region while adding significant entropy. */
#define ASLR_KSTACK_RANDOM_PAGES 8

/* Maximum random shift for PIE (ET_DYN) ELF base address (in pages).
 * PIE binaries get a randomized load base; non-PIE (ET_EXEC) load at
 * their fixed p_vaddr. Up to 256 pages = 1MB random offset provides
 * meaningful entropy while keeping the binary within a predictable
 * region of the user address space. */
#define ASLR_PIE_RANDOM_PAGES 256

/* Query whether ASLR is globally enabled */
static inline int aslr_is_enabled(void) {
    extern int aslr_disabled;
    return !aslr_disabled;
}

/* Get random bytes for ASLR */
void aslr_init(void);

/* Return a random number of pages (0..max_pages) for stack randomization */
uint64_t aslr_stack_offset(void);

/* Return a random number of pages for mmap base randomization */
uint64_t aslr_mmap_offset(void);

/* Return a random number of pages for brk base randomization */
uint64_t aslr_brk_offset(void);

/* Return random bytes for userspace AT_RANDOM (stack canary seed) */
void aslr_get_at_random(uint8_t buf[16]);

/* Return a random number of pages (0..ASLR_MODULE_RANDOM_PAGES) for
 * randomizing the kernel module loading base address. */
uint64_t aslr_module_offset(void);

/* Return a random number of pages (0..ASLR_KSTACK_RANDOM_PAGES) for
 * randomizing the per-CPU kernel stack base offset. Returns 0 when
 * ASLR is disabled or called before PRNG is seeded. */
uint64_t aslr_kernel_stack_offset(void);

/* Seed the PRNG with additional entropy from timing */
void aslr_add_entropy(uint64_t entropy);

/* Return a random number of pages (0..max_pages) as a base offset.
 * Returns 0 if ASLR is globally disabled. */
uint64_t aslr_get_random_offset(uint64_t max_pages);

#endif
