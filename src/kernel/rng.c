/*
 * rng.c — Random Number Generator
 *
 * Uses xorshift64 PRNG seeded from timer_get_ticks().
 * Simple, fast, non-cryptographic random number generation.
 */

#include "rng.h"
#include "timer.h"
#include "printf.h"

static uint64_t g_rng_state = 0;

void rng_init(void) {
    /* Seed from timer ticks — captures boot-time jitter */
    g_rng_state = timer_get_ticks();
    if (g_rng_state == 0) g_rng_state = 1;

    /* Mix in some CPU jitter by XORing with the frame pointer / stack address */
    uint64_t stack_var;
    g_rng_state ^= (uint64_t)&stack_var;

    /* Initialize with a few warm-up rounds */
    for (int i = 0; i < 10; i++) {
        (void)rng_get_u64();
    }

    kprintf("[OK] RNG initialized\n");
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

uint32_t rng_get_u32(void) {
    return (uint32_t)xorshift64(&g_rng_state);
}

uint64_t rng_get_u64(void) {
    return xorshift64(&g_rng_state);
}

void rng_fill_buf(void *buf, uint32_t len) {
    uint8_t *bytes = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        bytes[i] = (uint8_t)rng_get_u32();
    }
}
