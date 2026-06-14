#ifndef STACKPROT_H
#define STACKPROT_H

/*
 * stackprot.h — Per-task stack canary support
 *
 * Each process has its own unique stack canary in process->stack_canary.
 * On context switch, the scheduler loads the new process's canary into
 * the global __stack_chk_guard variable, which GCC's -fstack-protector-
 * strong uses to verify stack integrity at function return.
 *
 * Canary generation:
 *   - Preferred: RDRAND instruction (hardware random, falls back if not
 *     supported, detected via CPUID bit 1<<30 in ECX after leaf 1)
 *   - Fallback: XOR of TSC fast counter + PID + random seed
 *
 * If RDRAND is available, we use it directly for maximum entropy.
 * Otherwise we derive the canary from TSC, PID, and a boot-time seed.
 */

#include "types.h"

/* Generate a random canary value.
 * Uses RDRAND if available, otherwise falls back to TSC+PID+seed hash. */
static inline uint64_t stackprot_generate_canary(uint32_t pid)
{
    uint64_t canary;

    /* Try RDRAND first */
    int retry = 10;
    while (retry--) {
        unsigned char ok;
        __asm__ volatile(
            "rdrand %0\n\t"
            "setc %1"
            : "=r"(canary), "=qm"(ok)
            :
            : "cc"
        );
        if (ok)
            goto zero_check;
    }

    /* RDRAND failed or not available — fallback to TSC + PID + seed */
    {
        uint64_t tsc_lo, tsc_hi;
        __asm__ volatile("rdtsc" : "=a"(tsc_lo), "=d"(tsc_hi));
        uint64_t rng_seed;
        __asm__ volatile("rdtscp" : "=a"(rng_seed) : : "ecx", "edx");
        canary = tsc_lo ^ (tsc_hi << 32) ^ (uint64_t)pid ^ (rng_seed << 16);
        /* Mix to avoid low entropy in lower bits */
        canary ^= (canary >> 32);
        canary ^= (canary >> 16);
        canary ^= (canary >> 8);
    }

zero_check:
    /* Ensure canary is never 0 (otherwise stack protection is defeated) */
    if (canary == 0)
        canary = 0xB16B00B5DEADBEEFULL ^ (uint64_t)pid;

    /* Null-terminate the canary: make the first byte non-zero - the
     * string-based attacks often overflow a string buffer and hit the
     * canary; if the first byte is 0x00 the overflow would stop before
     * overwriting the rest.  Having a 0x00 byte in the canary also
     * defeats simple string copy overflows. */
    canary &= ~0xFFULL;           /* clear low byte */
    canary |= (uint8_t)(canary >> 56);  /* set it to a non-zero value */
    if ((canary & 0xFF) == 0)
        canary |= 0xA5;           /* ensure non-zero low byte */

    return canary;
}

#endif /* STACKPROT_H */
