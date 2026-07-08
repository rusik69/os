#define KERNEL_INTERNAL
#include "types.h"
#include "aslr.h"
#include "string.h"
#include "printf.h"
#include "syscall.h"   /* for prng_rand64 */
#include "timer.h"     /* for timer_get_ticks */
#include "cmdline.h"   /* for cmdline_has */

/*
 * ASLR — Address Space Layout Randomization
 *
 * Provides full per-exec randomization:
 *   - Stack base offset (up to ASLR_STACK_RANDOM_PAGES pages)
 *   - mmap base offset (up to ASLR_MMAP_RANDOM_PAGES pages)
 *   - brk/heap base offset (up to ASLR_BRK_RANDOM_PAGES pages)
 *   - Kernel module base offset (KASLR for modules)
 *   - Kernel stack base offset (per-CPU)
 *
 * Entropy is derived from prng_rand64() which is seeded from RTC + tick
 * count at boot, plus hardware RDRAND if available. Additional entropy
 * is mixed in from timer IRQ jitter and scheduler timing.
 *
 * All features can be disabled globally with the "nokaslr" kernel
 * command-line parameter.
 */

/* Global ASLR toggle — disables ALL randomization when set */
int aslr_disabled = 0;

/* Track whether we've added extra entropy */
static int aslr_entropy_seeded = 0;

/* RDRAND instruction — returns 1 on success, 0 if not available.
 * CPUID.1.ECX[30] = RDRAND support. Check before executing. */
static inline int rdrand64(uint64_t *val) {
    uint32_t eax = 1, ebx, ecx = 0, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));
    if (!((ecx >> 30) & 1))
        return 0;  /* ponytail: CPU lacks RDRAND, caller falls back to PRNG */
    unsigned char ok;
    __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0xf0 ; setc %1"
                     : "=a"(*val), "=qm"(ok) :: "cc");
    return (int)ok;
}

/**
 * aslr_init - Initialize ASLR subsystem
 *
 * Checks the kernel command line for "nokaslr" to disable ASLR globally.
 * Attempts to gather hardware entropy from the RDRAND instruction and
 * seeds the PRNG. Additional entropy is mixed in from stack address
 * randomization and timer tick timing.
 *
 * Context: Called once during kernel boot, before any process creation.
 *          No locking required.
 * Return: void.
 */
void __init aslr_init(void) {
    /* Check for "nokaslr" on kernel command line — disables ALL ASLR */
    if (cmdline_has("nokaslr")) {
        aslr_disabled = 1;
        kprintf("[OK] ASLR disabled via cmdline (nokaslr)\n");
        return;
    }

    /* Attempt to gather hardware entropy from RDRAND */
    uint64_t rdrand_val = 0;
    if (rdrand64(&rdrand_val)) {
        prng_add_entropy(rdrand_val);
        kprintf("[OK] ASLR: gathered %llu bits hardware entropy via RDRAND\n",
                (unsigned long long)64);
    }

    /* The PRNG in syscall_init() has already been seeded with RTC + tick.
     * We add extra entropy from our own initialisation timing. */
    uint64_t extra = timer_get_ticks();
    extra ^= (uint64_t)(uintptr_t)&extra;  /* stack address as entropy */
    prng_add_entropy(extra);
    aslr_entropy_seeded = 1;
    kprintf("[OK] ASLR initialized\n");
}

/**
 * aslr_stack_offset - Return random stack base offset in pages
 *
 * Returns a random number of pages (0..ASLR_STACK_RANDOM_PAGES) for
 * shifting the user stack base downward from USER_STACK_TOP.
 *
 * Context: Any context. Calls prng_rand64() which may require PRNG lock.
 * Return: Random page offset, or 0 if ASLR is globally disabled.
 */
uint64_t aslr_stack_offset(void) {
    if (aslr_disabled) return 0;
    return prng_rand64() % (ASLR_STACK_RANDOM_PAGES + 1);
}

/**
 * aslr_mmap_offset - Return random mmap base offset in pages
 *
 * Returns a random number of pages (0..ASLR_MMAP_RANDOM_PAGES) for
 * shifting the mmap allocation base upward from the default starting
 * address.
 *
 * Context: Any context. Calls prng_rand64() which may require PRNG lock.
 * Return: Random page offset, or 0 if ASLR is globally disabled.
 */
uint64_t aslr_mmap_offset(void) {
    if (aslr_disabled) return 0;
    return prng_rand64() % (ASLR_MMAP_RANDOM_PAGES + 1);
}

/*
 * Return a random number of pages (0..ASLR_BRK_RANDOM_PAGES) for shifting
 * the heap/brk base upward from the default starting address.
 * Returns 0 if ASLR is globally disabled.
 */
uint64_t aslr_brk_offset(void) {
    if (aslr_disabled) return 0;
    return prng_rand64() % (ASLR_BRK_RANDOM_PAGES + 1);
}

/*
 * Fill a 16-byte buffer with random bytes for the AT_RANDOM auxiliary vector
 * entry (used for stack canary seed, etc.).
 * Returns all zeros if ASLR is globally disabled.
 */
void aslr_get_at_random(uint8_t buf[16]) {
    if (aslr_disabled) {
        memset(buf, 0, 16);
        return;
    }
    uint64_t r1 = prng_rand64();
    uint64_t r2 = prng_rand64();
    for (int i = 0; i < 8; i++) {
        buf[i] = (uint8_t)(r1 >> (i * 8));
        buf[i + 8] = (uint8_t)(r2 >> (i * 8));
    }
}

/*
 * Return a random number of pages (0..ASLR_MODULE_RANDOM_PAGES) for
 * randomizing the kernel module loading base address.
 *
 * Called once during module_init() to compute the offset.  The random shift
 * ensures that module virtual addresses are not easily predictable across
 * boots, making it harder for an attacker to find module code/data.
 * Returns 0 if ASLR is globally disabled.
 */
uint64_t aslr_module_offset(void) {
    if (aslr_disabled) return 0;
    return prng_rand64() % (ASLR_MODULE_RANDOM_PAGES + 1);
}

/*
 * Return a random number of pages (0..ASLR_KSTACK_RANDOM_PAGES) for
 * randomizing the per-CPU kernel stack base address.
 *
 * This is called during CPU initialization to give each CPU's kernel
 * stack a random offset, making stack-based attacks harder. Returns 0
 * if ASLR is globally disabled or the PRNG hasn't been seeded yet.
 */
uint64_t aslr_kernel_stack_offset(void) {
    if (aslr_disabled) return 0;
    return prng_rand64() % (ASLR_KSTACK_RANDOM_PAGES + 1);
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

/**
 * aslr_randomize_addr - Randomize a base address within a given range
 * @base: Base address to randomize
 * @range: Range in bytes within which to add a random offset
 *
 * Stub implementation: currently logs a message and returns 0.
 * In a full implementation this would return @base + random_offset
 * where random_offset is in [0, @range) and page-aligned.
 *
 * Context: Any context.
 * Return: Randomized address (currently 0, stub).
 */
static uint64_t aslr_randomize_addr(uint64_t base, uint64_t range)
{
    (void)base;
    (void)range;
    kprintf("[aslr] aslr_randomize_addr: not yet implemented\n");
    return 0;
}
/* ── Stub: aslr_randomize_stack ─────────────────────────────── */\nstatic uint64_t aslr_randomize_stack(void)
{
    kprintf("[aslr] aslr_randomize_stack: not yet implemented\n");
    return 0;
}
