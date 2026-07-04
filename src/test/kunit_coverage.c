/*
 * kunit_coverage.c — KUnit gcov-style coverage tracking.
 *
 * Implements a lightweight in-kernel coverage tracker that records
 * which functions / code paths are exercised during KUnit test runs.
 *
 * The table is a fixed-size array of (name, hits, in_use) entries
 * protected by a spinlock.  Debugfs entries under /sys/kernel/debug/kunit/
 * allow the user to read the current coverage report and reset counters
 * between test runs.
 *
 * D250 task 15: KUnit coverage tracking (gcov-style integration)
 */

#define KERNEL_INTERNAL
#include "kunit_coverage.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "export.h"

/* ── Internal state ──────────────────────────────────────────────── */

/** Fixed-size coverage table. */
static struct kunit_cov_entry g_cov_table[KUNIT_COV_MAX_ENTRIES];

/** Spinlock protecting the coverage table. */
static spinlock_t g_cov_lock;

/* ── Helpers ─────────────────────────────────────────────────────── */

static uint32_t cov_total_hits(void)
{
    uint32_t total = 0;
    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        if (g_cov_table[i].in_use)
            total += g_cov_table[i].hits;
    }
    return total;
}

/* ── Public API ──────────────────────────────────────────────────── */

void kunit_coverage_init(void)
{
    spinlock_init(&g_cov_lock);
    memset(g_cov_table, 0, sizeof(g_cov_table));
}

void kunit_coverage_reset(void)
{
    spinlock_acquire(&g_cov_lock);
    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        g_cov_table[i].hits = 0;
        g_cov_table[i].in_use = false;
        g_cov_table[i].name[0] = '\0';
    }
    spinlock_release(&g_cov_lock);
}

bool kunit_coverage_hit(const char *name)
{
    if (!name || !name[0])
        return false;

    spinlock_acquire(&g_cov_lock);

    /* Look for existing entry and increment */
    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        if (g_cov_table[i].in_use &&
            strcmp(g_cov_table[i].name, name) == 0) {
            g_cov_table[i].hits++;
            spinlock_release(&g_cov_lock);
            return true;
        }
    }

    /* Find a free slot and create new entry */
    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        if (!g_cov_table[i].in_use) {
            strncpy(g_cov_table[i].name, name,
                    KUNIT_COV_NAME_LEN - 1);
            g_cov_table[i].name[KUNIT_COV_NAME_LEN - 1] = '\0';
            g_cov_table[i].hits = 1;
            g_cov_table[i].in_use = true;
            spinlock_release(&g_cov_lock);
            return true;
        }
    }

    /* Table full */
    spinlock_release(&g_cov_lock);
    return false;
}
EXPORT_SYMBOL(kunit_coverage_hit);

int kunit_coverage_report(char *buf, int max_len)
{
    int pos = 0;

    if (!buf || max_len <= 0)
        return 0;

    spinlock_acquire(&g_cov_lock);

    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "KUnit Coverage Report\n"
        "=====================\n"
        "Active coverage points: %d\n"
        "Total hits:            %u\n\n",
        kunit_coverage_active_count(), cov_total_hits());

    if (pos < 0) pos = 0;

    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        if (!g_cov_table[i].in_use)
            continue;

        int remaining = max_len - pos;
        if (remaining < 60) {
            /* Room for one more line plus truncation marker? */
            pos += snprintf(buf + pos, (size_t)(remaining > 0 ? remaining : 0),
                "  ... (truncated, %d entries)\n",
                kunit_coverage_active_count());
            break;
        }

        pos += snprintf(buf + pos, (size_t)(remaining),
            "  %-40s  %5u\n",
            g_cov_table[i].name, g_cov_table[i].hits);
        if (pos < 0) pos = 0;
    }

    spinlock_release(&g_cov_lock);

    if (pos >= max_len)
        pos = max_len - 1;
    if (pos < 0)
        pos = 0;

    return pos;
}

int kunit_coverage_active_count(void)
{
    int count = 0;
    for (int i = 0; i < KUNIT_COV_MAX_ENTRIES; i++) {
        if (g_cov_table[i].in_use)
            count++;
    }
    return count;
}

uint32_t kunit_coverage_total_hits(void)
{
    spinlock_acquire(&g_cov_lock);
    uint32_t total = cov_total_hits();
    spinlock_release(&g_cov_lock);
    return total;
}
