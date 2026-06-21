/*
 * kaslr.c — Kernel Address Space Layout Randomization
 *
 * Generates a random 2MB-aligned offset for the kernel base address.
 * This offset is chosen once during early boot and fixed for the session.
 *
 * Entropy sources (in priority order):
 *   1. RDRAND instruction (hardware random number generator)
 *   2. TSC (Timestamp Counter) + stack address entropy
 *
 * The KASLR offset adjusts the kernel's high-half virtual base, making
 * the location of kernel code/data unpredictable across boots.
 *
 * Disable with "nokaslr" on the kernel command line.
 */

#define KERNEL_INTERNAL
#include "kaslr.h"
#include "aslr.h"       /* for aslr_disabled */
#include "printf.h"
#include "timer.h"      /* for timer_get_ticks */

/* ── Global KASLR offset (extern in kaslr.h) ────────────────────── */
uint64_t kaslr_offset = 0;

/* ── Has the offset been generated? (for one-shot generation) ───── */
static int kaslr_seeded = 0;

/* ── RDRAND instruction helper ──────────────────────────────────────
 * Returns 1 on success (valid random value in *val), 0 if RDRAND
 * is not available or the instruction failed.
 * CPUID.1.ECX[30] = RDRAND support. Check before executing. */
static inline int rdrand64(uint64_t *val) {
    uint32_t eax = 1, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    if (!((ecx >> 30) & 1))
        return 0;  /* ponytail: CPU lacks RDRAND, caller falls back to TSC+stack */
    unsigned char ok;
    /* RDRAND is encoded as: 0x48 0x0f 0xc7 0xf0 (REX.W + opcode + ModRM)
     * Sets CF=1 on success, CF=0 if no random data available. */
    __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0xf0 ; setc %1"
                     : "=a"(*val), "=qm"(ok) :: "cc");
    return (int)ok;
}

/* ── TSC (Timestamp Counter) reader ───────────────────────────────── */
static inline uint64_t read_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

/* ── Generate the KASLR offset ───────────────────────────────────────
 * Called once during early boot.  Subsequent calls return cached value.
 *
 * Uses RDRAND hardware randomness if available; falls back to
 * TSC + timer tick entropy otherwise.
 *
 * The returned value is always 2MB-aligned and in [0, KASLR_MAX_OFFSET].
 */
uint64_t kaslr_get_offset(void) {
    if (kaslr_seeded)
        return kaslr_offset;

    /* If ASLR is globally disabled (cmdline "nokaslr"), offset = 0 */
    if (aslr_disabled) {
        kaslr_offset = 0;
        kaslr_seeded = 1;
        return 0;
    }

    uint64_t entropy = 0;

    /* Try RDRAND first (hardware entropy, preferred) */
    if (rdrand64(&entropy)) {
        /* Success — use RDRAND value directly */
    } else {
        /* Fallback: mix TSC and timer ticks for entropy.
         * Read TSC twice with a small delay to amplify timing jitter. */
        uint64_t tsc1 = read_tsc();
        /* Small delay loop to amplify jitter */
        for (volatile int i = 0; i < 100; i++) {}
        uint64_t tsc2 = read_tsc();

        /* Mix in timer ticks if available */
        uint64_t ticks = timer_get_ticks();

        entropy = tsc1 ^ (tsc2 << 17) ^ (tsc2 >> 13) ^ ticks;
        /* Mix in the stack address as additional per-boot entropy */
        entropy ^= (uint64_t)(uintptr_t)&entropy;
    }

    /* Constrain to range [0, KASLR_MAX_OFFSET] with 2MB alignment */
    uint64_t max_steps = KASLR_MAX_OFFSET / KASLR_ALIGN;
    uint64_t step = entropy % (max_steps + 1);
    kaslr_offset = step * KASLR_ALIGN;

    kaslr_seeded = 1;
    return kaslr_offset;
}

/* ── Initialisation ──────────────────────────────────────────────────
 * Called early in kernel_main(), after serial/VGA is up.
 * Logs status and stores the offset in the global variable.
 */
void kaslr_init(void) {
    uint64_t off = kaslr_get_offset();

    if (off == 0) {
        if (aslr_disabled)
            kprintf("[KASLR] Disabled via cmdline (nokaslr)\n");
        else
            kprintf("[KASLR] Offset = 0 (no randomization)\n");
    } else {
        kprintf("[KASLR] Offset = 0x%llx (%llu MB, %llu steps of 2MB)\n",
                (unsigned long long)off,
                (unsigned long long)(off / (1024 * 1024)),
                (unsigned long long)(off / KASLR_ALIGN));
    }
}

/* ── Stub: kaslr_randomize ─────────────────────────────── */
uint64_t kaslr_randomize(void)
{
    kprintf("[kaslr] kaslr_randomize: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kaslr_get_random_addr ─────────────────────────────── */
uint64_t kaslr_get_random_addr(uint64_t base, uint64_t size)
{
    (void)base;
    (void)size;
    kprintf("[kaslr] kaslr_get_random_addr: not yet implemented\n");
    return -ENOSYS;
}
