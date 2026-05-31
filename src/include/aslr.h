#ifndef ASLR_H
#define ASLR_H

#include "types.h"

/*
 * ASLR (Address Space Layout Randomization) for user-space processes.
 *
 * Provides:
 *   - Random stack base offset per process (up to ASLR_STACK_RANDOM_BYTES)
 *   - Random mmap base for anonymous mappings
 *   - Random brk/ELF base offset
 *
 * The entropy is derived from the kernel's prng_rand64() which is seeded
 * from RTC + tick count at boot.
 */

/* Maximum random shift for user stack top (in pages) */
#define ASLR_STACK_RANDOM_PAGES  16   /* up to 64KB random offset */

/* Maximum random shift for mmap base (in pages) */
#define ASLR_MMAP_RANDOM_PAGES   256  /* up to 1MB random offset */

/* Maximum random shift for brk/data segment (in pages) */
#define ASLR_BRK_RANDOM_PAGES    32

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

/* Seed the PRNG with additional entropy from timing */
void aslr_add_entropy(uint64_t entropy);

#endif
