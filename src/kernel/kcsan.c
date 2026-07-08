/*
 * kcsan.c — Kernel Concurrency Sanitizer (KCSAN) stub
 *
 * KCSAN is a dynamic data race detector for the Linux kernel.  This
 * implementation provides a lightweight framework that instruments
 * memory accesses and detects concurrent (unsynchronized) accesses
 * to the same memory location.
 *
 * Features:
 *   - Watchpoint-based race detection for memory accesses
 *   - Tracks which CPUs accessed which addresses
 *   - Reports races when conflicting accesses are detected without
 *     proper synchronization (locking, atomics, etc.)
 *   - Debugfs interface for control and statistics
 *
 * This is a simplified educational implementation — it demonstrates
 * the core concepts without the full complexity of the Linux KCSAN.
 *
 * Item 138 — KCSAN: Kernel Concurrency Sanitizer
 */

#define KERNEL_INTERNAL
#include "types.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "smp.h"
#include "timer.h"
#include "debugfs.h"

/* ── Configuration ──────────────────────────────────────────────────── */

#define KCSAN_MAX_WATCHPOINTS    64
#define KCSAN_WATCHPOINT_TIMEOUT 100  /* ms before eviction */

/* ── Watchpoint types ────────────────────────────────────────────────── */

#define KCSAN_ACCESS_READ  0
#define KCSAN_ACCESS_WRITE 1
#define KCSAN_ACCESS_ATOMIC 2

/* ── Watchpoint descriptor ──────────────────────────────────────────── */

struct kcsan_watchpoint {
    volatile uint32_t in_use;          /* 1 if this slot is active */
    volatile uint64_t addr;            /* Address being watched */
    volatile uint64_t size;            /* Access size (1, 2, 4, 8) */
    volatile int      access_type;     /* KCSAN_ACCESS_* */
    volatile uint32_t cpu_id;          /* CPU that set the watchpoint */
    volatile uint64_t timestamp;       /* When the watchpoint was set */
    volatile int      observed_race;   /* Set to 1 if a race was detected */
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct kcsan_watchpoint kcsan_watchpoints[KCSAN_MAX_WATCHPOINTS];
static spinlock_t kcsan_lock;
static int        kcsan_initialized = 0;
static int        kcsan_enabled = 1;   /* Enabled by default */
static volatile uint64_t kcsan_races_detected = 0;
static volatile uint64_t kcsan_accesses_tracked = 0;

/* ── Internal helpers ───────────────────────────────────────────────── */

/*
 * Check if any existing watchpoint conflicts with the given access.
 * A conflict occurs when:
 *   - Same address range (overlapping)
 *   - At least one of the accesses is a WRITE
 *   - Different CPUs
 *   - No synchronization (lock held, etc.) between them
 */
static int kcsan_check_watchpoints(uint64_t addr, uint64_t size,
                                    int access_type, uint32_t cpu_id)
{
    for (int i = 0; i < KCSAN_MAX_WATCHPOINTS; i++) {
        struct kcsan_watchpoint *wp = &kcsan_watchpoints[i];

        if (!wp->in_use)
            continue;

        /* Check for address range overlap */
        uint64_t wp_end = wp->addr + wp->size;
        uint64_t acc_end = addr + size;

        if (addr < wp_end && acc_end > wp->addr) {
            /* Overlapping access */
            if (access_type == KCSAN_ACCESS_WRITE ||
                wp->access_type == KCSAN_ACCESS_WRITE) {
                /* At least one is a write — potential race */
                if (wp->cpu_id != cpu_id) {
                    /* Different CPUs — data race! */
                    wp->observed_race = 1;
                    return 1;  /* Race detected */
                }
            }
        }
    }

    return 0;
}

/*
 * Set a watchpoint for the given memory access.
 */
static void kcsan_set_watchpoint(uint64_t addr, uint64_t size,
                                  int access_type, uint32_t cpu_id)
{
    for (int i = 0; i < KCSAN_MAX_WATCHPOINTS; i++) {
        struct kcsan_watchpoint *wp = &kcsan_watchpoints[i];

        if (!wp->in_use) {
            wp->in_use = 1;
            wp->addr = addr;
            wp->size = size;
            wp->access_type = access_type;
            wp->cpu_id = cpu_id;
            wp->timestamp = timer_get_ticks();
            wp->observed_race = 0;
            return;
        }

        /* Evict stale watchpoints */
        if (wp->in_use) {
            uint64_t age = timer_get_ticks() - wp->timestamp;
            /* Convert ticks to ms (assuming ~100 Hz timer) */
            if (age > (KCSAN_WATCHPOINT_TIMEOUT * 100 / 1000)) {
                /* Stale watchpoint, reuse */
                wp->addr = addr;
                wp->size = size;
                wp->access_type = access_type;
                wp->cpu_id = cpu_id;
                wp->timestamp = timer_get_ticks();
                wp->observed_race = 0;
                return;
            }
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

static void __init kcsan_init(void)
{
    if (kcsan_initialized) return;

    memset(kcsan_watchpoints, 0, sizeof(kcsan_watchpoints));
    spinlock_init(&kcsan_lock);
    kcsan_initialized = 1;
    kcsan_enabled = 1;

    kprintf("[KCSAN] Kernel Concurrency Sanitizer initialized\n");
    kprintf("[KCSAN] %d watchpoints, timeout=%d ms\n",
            KCSAN_MAX_WATCHPOINTS, KCSAN_WATCHPOINT_TIMEOUT);
}

/*
 * Instrument a memory access for race detection.
 *
 * Called by instrumented kernel code (via compiler instrumentation
 * or manual annotations) for every memory access when KCSAN is enabled.
 *
 * @addr:        Address being accessed.
 * @size:        Access size in bytes (1, 2, 4, 8).
 * @access_type: KCSAN_ACCESS_READ, KCSAN_ACCESS_WRITE, or KCSAN_ACCESS_ATOMIC.
 */
static void kcsan_check_access(uint64_t addr, uint64_t size, int access_type)
{
    if (!kcsan_initialized || !kcsan_enabled)
        return;

    if (access_type == KCSAN_ACCESS_ATOMIC)
        return;  /* Atomic accesses are never races */

    uint32_t cpu_id = smp_get_cpu_id();
    kcsan_accesses_tracked++;

    spinlock_acquire(&kcsan_lock);

    /* Check if this access races with an existing watchpoint */
    if (kcsan_check_watchpoints(addr, size, access_type, cpu_id)) {
        kcsan_races_detected++;
        kprintf("[KCSAN] DATA RACE detected at 0x%lx (CPU%u, %s, size=%llu)\n",
                (unsigned long)addr, cpu_id,
                (access_type == KCSAN_ACCESS_READ) ? "READ" : "WRITE",
                (unsigned long long)size);
    }

    /* Set a new watchpoint for this access */
    if (access_type == KCSAN_ACCESS_WRITE) {
        /* Only watch write accesses to detect future conflicting reads/writes */
        kcsan_set_watchpoint(addr, size, access_type, cpu_id);
    }

    spinlock_release(&kcsan_lock);
}

/*
 * Enable or disable KCSAN race detection.
 */
static void kcsan_enable(void)
{
    kcsan_enabled = 1;
    kprintf("[KCSAN] Enabled\n");
}

static void kcsan_disable(void)
{
    kcsan_enabled = 0;
    kprintf("[KCSAN] Disabled\n");
}

static int kcsan_is_enabled(void)
{
    return kcsan_enabled;
}

/*
 * Get KCSAN statistics.
 */
static void kcsan_get_stats(uint64_t *races, uint64_t *tracked)
{
    if (races)   *races   = kcsan_races_detected;
    if (tracked) *tracked = kcsan_accesses_tracked;
}

/*
 * Reset KCSAN state (clear watchpoints and statistics).
 */
static void kcsan_reset(void)
{
    spinlock_acquire(&kcsan_lock);
    memset(kcsan_watchpoints, 0, sizeof(kcsan_watchpoints));
    kcsan_races_detected = 0;
    kcsan_accesses_tracked = 0;
    spinlock_release(&kcsan_lock);
    kprintf("[KCSAN] Reset\n");
}

/*
 * Debugfs read callback: print KCSAN state.
 */
static void kcsan_debugfs_read(char *buf, int *len)
{
    uint64_t races, tracked;
    kcsan_get_stats(&races, &tracked);

    *len = snprintf(buf, 256,
                    "KCSAN: enabled=%d races=%llu tracked=%llu\n",
                    kcsan_enabled,
                    (unsigned long long)races,
                    (unsigned long long)tracked);
}

/* ── kcsan_check_read ─────────────────────────────────────────────────── */
static void kcsan_check_read(uint64_t addr, uint64_t size)
{
    kcsan_check_access(addr, size, KCSAN_ACCESS_READ);
}

/* ── kcsan_check_write ────────────────────────────────────────────────── */
static void kcsan_check_write(uint64_t addr, uint64_t size)
{
    kcsan_check_access(addr, size, KCSAN_ACCESS_WRITE);
}

/* ── kcsan_atomic_check ───────────────────────────────────────────────── */
/*
 * Check an atomic memory access for race conditions.
 * Atomic accesses are inherently safe against data races on their own
 * (they are serialized), but they can still race with non-atomic accesses.
 * This function checks whether any existing watchpoint conflicts with
 * this atomic access.
 */
static void kcsan_atomic_check(uint64_t addr, uint64_t size, int is_write)
{
    if (!kcsan_initialized || !kcsan_enabled)
        return;

    uint32_t cpu_id = smp_get_cpu_id();

    spinlock_acquire(&kcsan_lock);

    /* Check if this atomic access races with existing watchpoints.
     * Atomic accesses can still race if a non-atomic access from
     * another CPU touches the same address. */
    int access_type = is_write ? KCSAN_ACCESS_WRITE : KCSAN_ACCESS_READ;
    if (kcsan_check_watchpoints(addr, size, access_type, cpu_id)) {
        kcsan_races_detected++;
        kprintf("[KCSAN] DATA RACE (atomic vs non-atomic) at 0x%lx"
                " (CPU%u, %s, size=%llu)\n",
                (unsigned long)addr, cpu_id,
                is_write ? "ATOMIC_WRITE" : "ATOMIC_READ",
                (unsigned long long)size);
    }

    /* Do NOT set watchpoint for atomic accesses — they are
     * self-synchronizing on their own, but we still check them
     * against existing watchpoints. */

    spinlock_release(&kcsan_lock);
}
