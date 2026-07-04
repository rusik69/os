/*
 * kunit_regression.c — KUnit regression database (baseline comparison).
 *
 * Stores KUnit test result snapshots and provides comparison against
 * saved baselines to detect regressions (new failures, increased
 * failure counts, coverage drops).
 *
 * Exposed via debugfs at /sys/kernel/debug/kunit/regression:
 *   echo "save <label>"   — Save current results as baseline "<label>"
 *   echo "compare <label>" — Compare current results against baseline
 *   echo "list"           — List all saved baselines
 *   echo "clear"          — Clear all saved baselines
 *   cat /sys/kernel/debug/kunit/regression  — Shows comparison report
 *     against the last compared baseline, or list if none compared yet.
 *
 * D250 task 16: KUnit regression database (compare against baseline)
 */

#define KERNEL_INTERNAL
#include "kunit_regression.h"
#include "kunit_coverage.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "export.h"

/* External KUnit counters (defined in kunit.c) */
extern int kunit_passed_count(void);
extern int kunit_failed_count(void);
extern int kunit_total_failures(void);
extern int kunit_coverage_active_count(void);
extern uint32_t kunit_coverage_total_hits(void);

/* ── Internal state ──────────────────────────────────────────────── */

/** Stored baselines (fixed-size array, protected by spinlock). */
static struct kunit_regression_baseline
    g_baselines[KUNIT_REGRESSION_MAX_BASELINES];

/** Spinlock protecting the baseline table. */
static spinlock_t g_regression_lock;

/** Label used for the last "compare" command (for read callback). */
static char g_last_compare_label[KUNIT_REGRESSION_LABEL_LEN];
static bool g_last_compare_active = false;

/** Cached comparison report (updated on each "compare" command). */
static char g_last_report[KUNIT_REGRESSION_REPORT_MAX];

/* ── Helpers ─────────────────────────────────────────────────────── */

/** Find a baseline by label (case-sensitive).  Returns index or -1. */
static int find_baseline(const char *label)
{
    if (!label || !label[0])
        return -1;

    for (int i = 0; i < KUNIT_REGRESSION_MAX_BASELINES; i++) {
        if (g_baselines[i].in_use &&
            strcmp(g_baselines[i].label, label) == 0) {
            return i;
        }
    }
    return -1;
}

/** Find a free slot.  Returns index or -1 if full. */
static int find_free_slot(void)
{
    for (int i = 0; i < KUNIT_REGRESSION_MAX_BASELINES; i++) {
        if (!g_baselines[i].in_use)
            return i;
    }
    return -1;
}

/* ── Public API ──────────────────────────────────────────────────── */

void kunit_regression_init(void)
{
    spinlock_init(&g_regression_lock);
    memset(g_baselines, 0, sizeof(g_baselines));
    g_last_compare_active = false;
    g_last_report[0] = '\0';
}

void kunit_regression_snapshot(struct kunit_regression_snapshot *snap)
{
    if (!snap)
        return;

    snap->tests_run          = kunit_passed_count() + kunit_failed_count();
    snap->tests_passed       = kunit_passed_count();
    snap->tests_failed       = kunit_failed_count();
    snap->assertions         = kunit_total_failures() +  /* approximate */
                                (kunit_passed_count() + kunit_failed_count());
    snap->assertion_failures = kunit_total_failures();
    snap->coverage_hits      = kunit_coverage_total_hits();
    snap->coverage_points    = kunit_coverage_active_count();
}

int kunit_regression_save(const char *label)
{
    if (!label || !label[0])
        return -2;

    if (strlen(label) >= KUNIT_REGRESSION_LABEL_LEN)
        return -2;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);

    /* Check for duplicate label */
    if (find_baseline(label) >= 0) {
        spinlock_irqsave_release(&g_regression_lock, irq_flags);
        return -3; /* duplicate label */
    }

    int slot = find_free_slot();
    if (slot < 0) {
        spinlock_irqsave_release(&g_regression_lock, irq_flags);
        return -1; /* table full */
    }

    kunit_regression_snapshot(&g_baselines[slot].snap);
    strncpy(g_baselines[slot].label, label,
            KUNIT_REGRESSION_LABEL_LEN - 1);
    g_baselines[slot].label[KUNIT_REGRESSION_LABEL_LEN - 1] = '\0';
    g_baselines[slot].in_use = true;

    spinlock_irqsave_release(&g_regression_lock, irq_flags);
    return 0;
}

int kunit_regression_compare(char *buf, int max_len, const char *label)
{
    if (!buf || max_len <= 0 || !label || !label[0])
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);

    int idx = find_baseline(label);
    if (idx < 0) {
        spinlock_irqsave_release(&g_regression_lock, irq_flags);
        int pos = snprintf(buf, (size_t)max_len,
            "[REGRESSION] Baseline '%s' not found\n", label);
        return pos < max_len ? pos : max_len - 1;
    }

    /* Snapshot current results */
    struct kunit_regression_snapshot current;
    kunit_regression_snapshot(&current);

    struct kunit_regression_snapshot *base = &g_baselines[idx].snap;
    int pos = 0;

    /* Header */
    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "========================================\n"
        "KUnit Regression Report — vs '%s'\n"
        "========================================\n\n",
        label);

    /* Test count comparison */
    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "  Tests run:    %4d  (baseline: %4d)  %s\n",
        current.tests_run, base->tests_run,
        current.tests_run >= base->tests_run ? "" : "⚠  FEWER TESTS");

    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "  Passed:       %4d  (baseline: %4d)  %s\n",
        current.tests_passed, base->tests_passed,
        current.tests_passed >= base->tests_passed ? "" : "⚠  FEWER PASSED");

    /* Failure delta — the key regression indicator */
    int fail_delta = current.tests_failed - base->tests_failed;
    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "  Failed:       %4d  (baseline: %4d)  %s%+d%s\n",
        current.tests_failed, base->tests_failed,
        fail_delta > 0 ? "⚠  REGRESSION: +" : "",
        fail_delta,
        fail_delta > 0 ? " failures" : "");

    /* Assertion comparison */
    int af_delta = current.assertion_failures - base->assertion_failures;
    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "  Assert fails: %4d  (baseline: %4d)  %s%+d\n",
        current.assertion_failures, base->assertion_failures,
        af_delta > 0 ? "⚠  REGRESSION: +" : "",
        af_delta);

    /* Coverage comparison */
    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "  Coverage:     %4u hits/%d pts  (baseline: %4u/%d)  %s\n",
        current.coverage_hits, current.coverage_points,
        base->coverage_hits, base->coverage_points,
        (current.coverage_hits >= base->coverage_hits ||
         current.coverage_points >= base->coverage_points)
         ? "" : "⚠  COVERAGE DROP");

    /* Regression verdict */
    int regressions = 0;
    if (fail_delta > 0) regressions++;
    if (af_delta > 0) regressions++;
    if (current.tests_passed < base->tests_passed) regressions++;
    if (current.coverage_hits < base->coverage_hits) regressions++;

    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "\n  Verdict: ");
    if (regressions == 0) {
        pos += snprintf(buf + pos,
            (size_t)(max_len - pos > 0 ? max_len - pos : 0),
            "✅  NO REGRESSIONS — baseline '%s' holds\n", label);
    } else {
        pos += snprintf(buf + pos,
            (size_t)(max_len - pos > 0 ? max_len - pos : 0),
            "❌  %d regression(s) detected against baseline '%s'\n",
            regressions, label);
    }

    pos += snprintf(buf + pos,
        (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "========================================\n");

    /* Update cached report */
    int copy_len = pos < KUNIT_REGRESSION_REPORT_MAX - 1
                    ? pos : KUNIT_REGRESSION_REPORT_MAX - 1;
    memcpy(g_last_report, buf, (size_t)copy_len);
    g_last_report[copy_len] = '\0';
    g_last_compare_active = true;
    strncpy(g_last_compare_label, label, KUNIT_REGRESSION_LABEL_LEN - 1);
    g_last_compare_label[KUNIT_REGRESSION_LABEL_LEN - 1] = '\0';

    spinlock_irqsave_release(&g_regression_lock, irq_flags);

    if (pos >= max_len)
        pos = max_len - 1;
    if (pos < 0) pos = 0;
    return pos;
}

int kunit_regression_list(char *buf, int max_len)
{
    if (!buf || max_len <= 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);

    int pos = 0;

    /* Count active baselines */
    int count = 0;
    for (int i = 0; i < KUNIT_REGRESSION_MAX_BASELINES; i++) {
        if (g_baselines[i].in_use)
            count++;
    }

    pos += snprintf(buf + pos, (size_t)(max_len - pos > 0 ? max_len - pos : 0),
        "KUnit Regression Baselines: %d/%d slots used\n",
        count, KUNIT_REGRESSION_MAX_BASELINES);

    if (count == 0) {
        pos += snprintf(buf + pos,
            (size_t)(max_len - pos > 0 ? max_len - pos : 0),
            "  (no baselines saved — use 'save <label>' to create one)\n");
    } else {
        for (int i = 0; i < KUNIT_REGRESSION_MAX_BASELINES; i++) {
            if (!g_baselines[i].in_use)
                continue;

            struct kunit_regression_snapshot *s = &g_baselines[i].snap;
            pos += snprintf(buf + pos,
                (size_t)(max_len - pos > 0 ? max_len - pos : 0),
                "  %2d: %-16s  passed=%d  failed=%d  "
                "assert_fails=%d  cov=%u/%d\n",
                i, g_baselines[i].label,
                s->tests_passed, s->tests_failed,
                s->assertion_failures,
                s->coverage_hits, s->coverage_points);
        }
    }

    spinlock_irqsave_release(&g_regression_lock, irq_flags);

    if (pos >= max_len)
        pos = max_len - 1;
    if (pos < 0) pos = 0;
    return pos;
}

void kunit_regression_clear(void)
{
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);
    memset(g_baselines, 0, sizeof(g_baselines));
    g_last_compare_active = false;
    g_last_report[0] = '\0';
    spinlock_irqsave_release(&g_regression_lock, irq_flags);
}

int kunit_regression_count(void)
{
    int count = 0;
    for (int i = 0; i < KUNIT_REGRESSION_MAX_BASELINES; i++) {
        if (g_baselines[i].in_use)
            count++;
    }
    return count;
}

int kunit_regression_last_report(char *buf, int max_len)
{
    if (!buf || max_len <= 0)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);

    int len = 0;
    if (g_last_compare_active && g_last_report[0]) {
        int copy_len = (int)strlen(g_last_report);
        if (copy_len > max_len - 1)
            copy_len = max_len - 1;
        memcpy(buf, g_last_report, (size_t)copy_len);
        buf[copy_len] = '\0';
        len = copy_len;
    }

    spinlock_irqsave_release(&g_regression_lock, irq_flags);
    return len;
}

bool kunit_regression_has_report(void)
{
    uint64_t irq_flags;
    bool active;
    spinlock_irqsave_acquire(&g_regression_lock, &irq_flags);
    active = g_last_compare_active;
    spinlock_irqsave_release(&g_regression_lock, irq_flags);
    return active;
}
