#define KERNEL_INTERNAL
#include "types.h"
#include "aslr.h"
#include "printf.h"
#include "syscall.h"   /* for prng_rand64 */
#include "timer.h"     /* for timer_get_ticks */

/*
 * ASLR — Address Space Layout Randomization
 *
 * Provides full per-exec randomization:
 *   - Stack base offset (up to ASLR_STACK_RANDOM_PAGES pages)
 *   - mmap base offset (up to ASLR_MMAP_RANDOM_PAGES pages)
 *   - brk/heap base offset (up to ASLR_BRK_RANDOM_PAGES pages)
 *
 * Entropy is derived from prng_rand64() which is seeded from RTC + tick
 * count at boot. Additional entropy is mixed in from timer IRQ jitter
 * and scheduler timing.
 */

/* Track whether we've added extra entropy */
static int aslr_entropy_seeded = 0;

void aslr_init(void) {
    /* The PRNG in syscall_init() has already been seeded with RTC + tick.
     * We add extra entropy from our own initialisation timing. */
    uint64_t extra = timer_get_ticks();
    extra ^= (uint64_t)(uintptr_t)&extra;  /* stack address as entropy */
    prng_add_entropy(extra);
    aslr_entropy_seeded = 1;
    kprintf("[OK] ASLR initialized\n");
}

/*
 * Return a random number of pages (0..ASLR_STACK_RANDOM_PAGES) for shifting
 * the user stack base downward from USER_STACK_TOP.
 */
uint64_t aslr_stack_offset(void) {
    return prng_rand64() % (ASLR_STACK_RANDOM_PAGES + 1);
}

/*
 * Return a random number of pages (0..ASLR_MMAP_RANDOM_PAGES) for shifting
 * the mmap allocation base upward from the default starting address.
 */
uint64_t aslr_mmap_offset(void) {
    return prng_rand64() % (ASLR_MMAP_RANDOM_PAGES + 1);
}

/*
 * Return a random number of pages (0..ASLR_BRK_RANDOM_PAGES) for shifting
 * the heap/brk base upward from the default starting address.
 */
uint64_t aslr_brk_offset(void) {
    return prng_rand64() % (ASLR_BRK_RANDOM_PAGES + 1);
}

/*
 * Fill a 16-byte buffer with random bytes for the AT_RANDOM auxiliary vector
 * entry (used for stack canary seed, etc.).
 */
void aslr_get_at_random(uint8_t buf[16]) {
    uint64_t r1 = prng_rand64();
    uint64_t r2 = prng_rand64();
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(r1 >> (i * 8));
        buf[i + 8] = (uint8_t)(r2 >> (i * 8));
    }
}

/*
 * Mix additional entropy into the ASLR/PRNG pool.
 * Called from timing-sensitive contexts (timer IRQ, scheduler) to add
 * jitter-based entropy with low latency.
 */
void aslr_add_entropy(uint64_t entropy) {
    if (!aslr_entropy_seeded) return;
    /* XOR-based entropy mixing: fold in the new entropy */
    prng_add_entropy(entropy);
}
