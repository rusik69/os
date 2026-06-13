/*
 * fault_inject.c — Kernel memory allocation fault injection
 *
 * Provides a fault injection framework for testing error recovery paths
 * in kernel subsystems.  When enabled, kmalloc() can be made to fail
 * at configurable intervals or with configurable probability, allowing
 * developers to verify that all allocation failure paths are handled
 * correctly.
 *
 * Item 271: Fault injection — memory allocation failure testing
 *   - fail_kmalloc knob: Nth call fails; test error paths in all subsystems
 *   - fail_kmalloc_probability: fail with given percentage probability
 *   - Runtime control via /sys/kernel/debug/fault_inject/ and debugfs
 *
 * Usage:
 *   # Enable fault injection, fail every 5th kmalloc (interval mode)
 *   echo 5 > /sys/kernel/debug/fault_inject/fail_kmalloc
 *
 *   # Enable fault injection, fail with 50% probability
 *   echo 50 > /sys/kernel/debug/fault_inject/fail_probability
 *
 *   # Disable
 *   echo 0 > /sys/kernel/debug/fault_inject/fail_kmalloc
 *
 *   # View statistics
 *   cat /sys/kernel/debug/fault_inject/call_count
 *   cat /sys/kernel/debug/fault_inject/fail_count
 *
 * Item 140 — Fault injection with debugfs and probability-based failing
 */

#include "fault_inject.h"
#include "printf.h"
#include "string.h"
#include "spinlock.h"
#include "sysfs.h"
#include "debugfs.h"
#include "timer.h"      /* for timer_get_ticks (random seed) */
#include "stdlib.h"     /* for rand() */

/* ── Tunable parameters ─────────────────────────────────────────────── */

/* The fault interval: fail every N kmalloc calls.
 *   0  = disabled (no fault injection)
 *   1  = fail every kmalloc call
 *   N>1= fail every Nth call
 */
static volatile int g_fail_kmalloc_interval = 0;

/* Probability-based failing: fail with P% probability.
 *   0  = disabled
 *   1-100 = fail with this percentage probability
 *   Overrides interval mode when non-zero.
 */
static volatile int g_fail_kmalloc_probability = 0;

/* Total number of kmalloc calls observed by the fault injector. */
static volatile uint64_t g_kmalloc_call_count = 0;

/* Total number of injected failures. */
static volatile uint64_t g_fail_count = 0;

/* Random seed for probability mode */
static int g_random_seeded = 0;

/* Protects concurrent access to the fault injection state. */
static spinlock_t g_fi_lock;

/* ── Sysfs read/write callbacks ─────────────────────────────────────── */

/* Read handler for /sys/kernel/debug/fault_inject/fail_kmalloc
 * Returns the current interval as a decimal string. */
static int fi_read_interval(char *buf, uint32_t max_size, void *priv) {
    (void)priv;
    int interval;

    spinlock_acquire(&g_fi_lock);
    interval = g_fail_kmalloc_interval;
    spinlock_release(&g_fi_lock);

    int n = snprintf(buf, (int)max_size, "%d\n", interval);
    if (n < 0) return 0;
    if ((uint32_t)n >= max_size) return (int)max_size - 1;
    return n;
}

/* Write handler for /sys/kernel/debug/fault_inject/fail_kmalloc
 * Parses a decimal integer to set the fault interval.
 *   0  = disable
 *   N>0= fail every Nth call */
static int fi_write_interval(const char *data, uint32_t size, void *priv) {
    (void)priv;

    /* Parse the decimal value from the input buffer */
    const char *p = data;
    uint32_t pos = 0;
    while (pos < size && *p && (*p == ' ' || *p == '\t')) { p++; pos++; }

    int val = 0;
    int sign = 1;
    if (pos < size && *p == '-') { sign = -1; p++; pos++; }

    while (pos < size && *p >= '0' && *p <= '9') {
        val = val * 10 + (int)(*p - '0');
        p++;
        pos++;
    }

    val *= sign;

    spinlock_acquire(&g_fi_lock);
    if (val == 0) {
        /* Writing 0 disables fault injection */
        g_fail_kmalloc_interval = 0;
        kprintf("[FI] fault injection disabled\n");
    } else if (val > 0) {
        g_fail_kmalloc_interval = val;
        kprintf("[FI] fault injection enabled: fail every %d kmalloc calls\n", val);
    } else {
        /* Negative values also disable */
        g_fail_kmalloc_interval = 0;
        kprintf("[FI] fault injection disabled (negative value)\n");
    }
    spinlock_release(&g_fi_lock);

    return 0;
}

/* Read handler for /sys/kernel/debug/fault_inject/call_count */
static int fi_read_call_count(char *buf, uint32_t max_size, void *priv) {
    (void)priv;
    uint64_t cnt;

    spinlock_acquire(&g_fi_lock);
    cnt = g_kmalloc_call_count;
    spinlock_release(&g_fi_lock);

    int n = snprintf(buf, (int)max_size, "%llu\n", (unsigned long long)cnt);
    if (n < 0) return 0;
    if ((uint32_t)n >= max_size) return (int)max_size - 1;
    return n;
}

/* Read handler for /sys/kernel/debug/fault_inject/fail_count */
static int fi_read_fail_count(char *buf, uint32_t max_size, void *priv) {
    (void)priv;
    uint64_t cnt;

    spinlock_acquire(&g_fi_lock);
    cnt = g_fail_count;
    spinlock_release(&g_fi_lock);

    int n = snprintf(buf, (int)max_size, "%llu\n", (unsigned long long)cnt);
    if (n < 0) return 0;
    if ((uint32_t)n >= max_size) return (int)max_size - 1;
    return n;
}

/* Read handler for /sys/kernel/debug/fault_inject/fail_probability */
static int fi_read_probability(char *buf, uint32_t max_size, void *priv) {
    (void)priv;
    int prob;

    spinlock_acquire(&g_fi_lock);
    prob = g_fail_kmalloc_probability;
    spinlock_release(&g_fi_lock);

    int n = snprintf(buf, (int)max_size, "%d\n", prob);
    if (n < 0) return 0;
    if ((uint32_t)n >= max_size) return (int)max_size - 1;
    return n;
}

/* Write handler for /sys/kernel/debug/fault_inject/fail_probability */
static int fi_write_probability(const char *data, uint32_t size, void *priv) {
    (void)priv;

    const char *p = data;
    uint32_t pos = 0;
    while (pos < size && *p && (*p == ' ' || *p == '\t')) { p++; pos++; }

    int val = 0;
    while (pos < size && *p >= '0' && *p <= '9') {
        val = val * 10 + (int)(*p - '0');
        p++;
        pos++;
    }

    spinlock_acquire(&g_fi_lock);
    if (val <= 0) {
        g_fail_kmalloc_probability = 0;
        kprintf("[FI] probability fault injection disabled\n");
    } else if (val > 100) {
        g_fail_kmalloc_probability = 100;
        kprintf("[FI] probability fault injection: 100%%\n");
    } else {
        g_fail_kmalloc_probability = val;
        kprintf("[FI] probability fault injection: %d%%\n", val);
    }
    spinlock_release(&g_fi_lock);

    return 0;
}

/* ── Initialisation ─────────────────────────────────────────────────── */

/* Register the fault injection sysfs interface under
 * /sys/kernel/debug/fault_inject/. */
static void fault_inject_sysfs_init(void) {
    /* Create parent directories */
    if (sysfs_create_dir("/sys/kernel/debug") < 0) {
        /* /sys/kernel/debug may already exist — that's fine */
    }

    if (sysfs_create_dir("/sys/kernel/debug/fault_inject") < 0) {
        kprintf("[FI] failed to create /sys/kernel/debug/fault_inject/\n");
        return;
    }

    /* Create writable fail_kmalloc control file */
    if (sysfs_create_writable_file(
            "/sys/kernel/debug/fault_inject/fail_kmalloc",
            "0\n", NULL, fi_read_interval, fi_write_interval) < 0) {
        kprintf("[FI] failed to create fail_kmalloc sysfs entry\n");
    }

    /* Create read-only call_count file */
    if (sysfs_create_writable_file(
            "/sys/kernel/debug/fault_inject/call_count",
            "0\n", NULL, fi_read_call_count, NULL) < 0) {
        kprintf("[FI] failed to create call_count sysfs entry\n");
    }

    /* Create read-only fail_count file */
    if (sysfs_create_writable_file(
            "/sys/kernel/debug/fault_inject/fail_count",
            "0\n", NULL, fi_read_fail_count, NULL) < 0) {
        kprintf("[FI] failed to create fail_count sysfs entry\n");
    }

    /* Create writable fail_probability control file */
    if (sysfs_create_writable_file(
            "/sys/kernel/debug/fault_inject/fail_probability",
            "0\n", NULL, fi_read_probability, fi_write_probability) < 0) {
        kprintf("[FI] failed to create fail_probability sysfs entry\n");
    }

    kprintf("[OK] Fault injection: /sys/kernel/debug/fault_inject/\n");
}

/* Register debugfs entries for fault injection */
static void fault_inject_debugfs_init(void) {
    debugfs_init();

    /* Create debugfs entries under /sys/kernel/debug/ */
    debugfs_create_u32("fault_inject_interval", (uint32_t*)&g_fail_kmalloc_interval);
    debugfs_create_u32("fault_inject_probability", (uint32_t*)&g_fail_kmalloc_probability);

    kprintf("[FI] Debugfs interface registered\n");
}

void fault_inject_init(void) {
    spinlock_init(&g_fi_lock);
    g_fail_kmalloc_interval = 0;
    g_fail_kmalloc_probability = 0;
    g_kmalloc_call_count = 0;
    g_fail_count = 0;
    g_random_seeded = 0;

    fault_inject_sysfs_init();
    fault_inject_debugfs_init();
}

/* ── Public API ─────────────────────────────────────────────────────── */

void fault_inject_enable(int interval) {
    spinlock_acquire(&g_fi_lock);
    if (interval <= 0) {
        g_fail_kmalloc_interval = 0;
        kprintf("[FI] fault injection disabled\n");
    } else {
        g_fail_kmalloc_interval = interval;
        g_kmalloc_call_count = 0;
        g_fail_count = 0;
        kprintf("[FI] fault injection enabled: fail every %d kmalloc calls\n", interval);
    }
    spinlock_release(&g_fi_lock);
}

int fault_inject_should_fail_kmalloc(void) {
    int should_fail = 0;

    spinlock_acquire(&g_fi_lock);

    g_kmalloc_call_count++;

    /* Probability mode takes precedence if set */
    if (g_fail_kmalloc_probability > 0) {
        /* Seed the random number generator on first use */
        if (!g_random_seeded) {
            uint64_t seed = timer_get_ticks();
            /* Simple seed XOR with call count for variety */
            seed ^= (uint64_t)g_kmalloc_call_count;
            srand((unsigned int)(seed & 0xFFFFFFFF));
            g_random_seeded = 1;
        }

        /* Generate a random number 0-99 and compare to probability */
        int roll = rand() % 100;
        if (roll < g_fail_kmalloc_probability) {
            g_fail_count++;
            should_fail = 1;
        }
    } else if (g_fail_kmalloc_interval > 0) {
        /* Fail when call_count % interval == 0 */
        if (g_kmalloc_call_count % (uint64_t)g_fail_kmalloc_interval == 0) {
            g_fail_count++;
            should_fail = 1;
        }
    }

    spinlock_release(&g_fi_lock);

    return should_fail;
}

uint64_t fault_inject_get_fail_count(void) {
    uint64_t cnt;
    spinlock_acquire(&g_fi_lock);
    cnt = g_fail_count;
    spinlock_release(&g_fi_lock);
    return cnt;
}

uint64_t fault_inject_get_call_count(void) {
    uint64_t cnt;
    spinlock_acquire(&g_fi_lock);
    cnt = g_kmalloc_call_count;
    spinlock_release(&g_fi_lock);
    return cnt;
}
