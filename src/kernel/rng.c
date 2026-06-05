/*
 * rng.c — Random Number Generator
 *
 * Uses xorshift64 PRNG seeded from timer_get_ticks() and, if available,
 * hardware RNG instructions (RDRAND / RDSEED).
 * Simple, fast, non-cryptographic random number generation.
 */

#include "rng.h"
#include "timer.h"
#include "printf.h"

static uint64_t g_rng_state = 0;

/* ── RDRAND / RDSEED support ───────────────────────────────────────────
 *
 * RDRAND (Intel Ivy Bridge+, AMD Jaguar+) returns random numbers from
 * a DRBG (Deterministic Random Bit Generator) seeded by an on-chip
 * hardware entropy source.
 *
 * RDSEED (Intel Broadwell+, AMD Zen+) returns raw entropy directly
 * from the hardware entropy source, making it more suitable for
 * seeding cryptographic PRNGs.
 *
 * Both instructions set the carry flag (CF) to indicate success;
 * if CF=0 the value returned is not valid.
 *
 * CPUID detection:
 *   RDRAND: leaf 1, ECX bit 30
 *   RDSEED: leaf 7 (sub-leaf 0), EBX bit 18
 */

/* Execute the RDRAND instruction (64-bit variant).
 * Returns 1 on success (CF=1), 0 if the value is invalid. */
static inline int rdrand_u64(uint64_t *val) {
    unsigned char ok;
    __asm__ volatile("rdrand %0; setc %1"
                     : "=r" (*val), "=qm" (ok));
    return (int)ok;
}

/* Execute the RDSEED instruction (64-bit variant).
 * Returns 1 on success (CF=1), 0 if the value is invalid. */
static inline int rdseed_u64(uint64_t *val) {
    unsigned char ok;
    __asm__ volatile("rdseed %0; setc %1"
                     : "=r" (*val), "=qm" (ok));
    return (int)ok;
}

/* Cache for CPUID feature flags (initialised lazily). */
static int g_rdrand_detected = -1;  /* -1 = not yet checked */
static int g_rdseed_detected = -1;  /* -1 = not yet checked */

/* Check whether RDRAND is supported via CPUID leaf 1, ECX bit 30. */
static int detect_rdrand(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(1));
    return (ecx >> 30) & 1;
}

/* Check whether RDSEED is supported via CPUID leaf 7 (sub-leaf 0),
 * EBX bit 18. */
static int detect_rdseed(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(7), "c"(0));
    return (ebx >> 18) & 1;
}

int rng_hw_rdrand_available(void) {
    if (g_rdrand_detected < 0)
        g_rdrand_detected = detect_rdrand();
    return g_rdrand_detected;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void rng_init(void) {
    /* Seed from timer ticks — captures boot-time jitter */
    g_rng_state = timer_get_ticks();
    if (g_rng_state == 0) g_rng_state = 1;

    /* Mix in some CPU jitter by XORing with the frame pointer / stack address */
    uint64_t stack_var;
    g_rng_state ^= (uint64_t)&stack_var;

    /* Attempt to seed from hardware RNG (RDRAND / RDSEED) */
    int hw_words = rng_seed_from_hw(4, RNG_HW_PREFER_RDSEED);
    if (hw_words > 0) {
        kprintf("[OK] RNG initialized (hw-seeded with %d words from %s)\n",
                hw_words, (g_rdseed_detected > 0) ? "RDSEED" : "RDRAND");
    } else {
        kprintf("[OK] RNG initialized (software seed, no hwrng)\n");
    }

    /* Initialize with a few warm-up rounds to diffuse the seed */
    for (int i = 0; i < 10; i++) {
        (void)rng_get_u64();
    }
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

/*
 * rng_add_entropy — Mix external entropy into the RNG state.
 *
 * Each byte of input data is XORed into the low byte of the current
 * xorshift64 state, then a full xorshift64 round is performed to
 * diffuse the entropy across all 64 bits.  This gives each entropy
 * byte influence over the entire state within O(1) time.
 *
 * Calling this with zero-length data is a no-op.
 */
void rng_add_entropy(const void *data, uint32_t len) {
    if (!data || len == 0)
        return;

    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        /* Mix byte into state, then run one xorshift round */
        g_rng_state ^= (uint64_t)bytes[i];
        (void)xorshift64(&g_rng_state);
    }

    /* Extra diffusion: mix in the length as well */
    g_rng_state ^= (uint64_t)len;
    (void)xorshift64(&g_rng_state);
}

/* ── Seed from hardware RNG (RDRAND / RDSEED) ────────────────────────── */

int rng_seed_from_hw(int words, int flags) {
    if (words <= 0)
        return 0;

    /* Cap to a reasonable maximum to avoid hogging the CPU */
    if (words > 16)
        words = 16;

    /* Check availability lazily */
    if (g_rdrand_detected < 0)
        g_rdrand_detected = detect_rdrand();
    if (g_rdseed_detected < 0)
        g_rdseed_detected = detect_rdseed();

    int use_rdseed = (flags & RNG_HW_PREFER_RDSEED) && g_rdseed_detected;
    int obtained   = 0;
    int retries    = 0;
    const int MAX_RETRIES = 10;

    while (obtained < words && retries < MAX_RETRIES) {
        uint64_t val;
        int ok;

        if (use_rdseed) {
            ok = rdseed_u64(&val);
            /* If RDSEED fails transiently (rare), fall back to RDRAND */
            if (!ok && g_rdrand_detected) {
                ok = rdrand_u64(&val);
                use_rdseed = 0;  /* don't try RDSEED again this call */
            }
        } else if (g_rdrand_detected) {
            ok = rdrand_u64(&val);
        } else {
            break;  /* no hardware RNG available */
        }

        if (ok) {
            /* Mix the hardware random word into the RNG state */
            rng_add_entropy(&val, sizeof(val));
            obtained++;
            retries = 0;  /* reset retry count on success */
        } else {
            retries++;
        }
    }

    return obtained;
}
