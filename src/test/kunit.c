/*
 * kunit.c — Lightweight in-kernel unit test framework (KUnit-like).
 *
 * Provides a debugfs interface at /sys/kernel/debug/kunit/ for running
 * and inspecting kernel unit tests.
 *
 * Debugfs entries:
 *   run_all    - Write "1" to execute all registered test suites
 *   run/<name> - Write "1" to execute a specific suite by name
 *   results    - Read to get a summary of all test results
 *   status     - Read to get the current pass/fail counts
 *
 * Item 266: KUnit — kernel unit test framework
 */

#include "kunit.h"
#include "debugfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "export.h"

/* Internal state */

/* Registered test suites */
static struct kunit_suite *g_suites[KUNIT_MAX_SUITES];
static int g_suite_count = 0;
static spinlock_t g_kunit_lock;

/* Result tracking */
static int g_total_tests_run = 0;
static int g_total_tests_passed = 0;
static int g_total_tests_failed = 0;
static int g_total_assertions = 0;
static int g_total_assertion_fails = 0;

/* Filter pattern */
static char g_filter[KUNIT_MAX_NAME];
static int g_filter_active = 0;

/* ── Forward declarations ─────────────────────────────────────────── */
static int  count_cases(struct kunit_suite *suite);
static void kunit_run_all(void);
static int  kunit_filter_write(const char *buf, int len);

/* ── Registration ─────────────────────────────────────────────────── */

int kunit_register_suite(struct kunit_suite *suite)
{
    if (!suite || !suite->name || !suite->name[0])
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_kunit_lock, &irq_flags);

    /* Check for duplicate */
    for (int i = 0; i < g_suite_count; i++) {
        if (strcmp(g_suites[i]->name, suite->name) == 0) {
            spinlock_irqsave_release(&g_kunit_lock, irq_flags);
            return -1;
        }
    }

    if (g_suite_count >= KUNIT_MAX_SUITES) {
        spinlock_irqsave_release(&g_kunit_lock, irq_flags);
        return -1;
    }

    g_suites[g_suite_count++] = suite;

    spinlock_irqsave_release(&g_kunit_lock, irq_flags);
    return 0;
}

/* ── Assertion helpers ────────────────────────────────────────────── */

void kunit_do_pass(struct kunit *test)
{
    (void)test;
    g_total_assertions++;
}

void kunit_do_fail(struct kunit *test, const char *file, int line,
                   const char *fmt, ...)
{
    if (!test) return;
    test->status = KUNIT_FAILURE;
    test->failures++;
    g_total_assertions++;
    g_total_assertion_fails++;

    /* Format and print the failure message */
    kprintf("[KUNIT] FAIL: %s:%d: ", file, line);
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    vkprintf(fmt, args);
    __builtin_va_end(args);
    kprintf("\n");
}

/* ── Test execution ───────────────────────────────────────────────── */

/* Check if a name matches the current filter (simple substring match) */
static int kunit_name_matches_filter(const char *name)
{
    if (!g_filter_active || !g_filter[0])
        return 1; /* no filter = match all */
    if (!name)
        return 0;
    return (strstr(name, g_filter) != NULL);
}

/* Run a single test case within a suite */
static void kunit_run_case(struct kunit_suite *suite, struct kunit_case *kc)
{
    struct kunit test_ctx;
    memset(&test_ctx, 0, sizeof(test_ctx));
    test_ctx.name = kc->name ? kc->name : "unnamed";
    test_ctx.status = KUNIT_SUCCESS;
    test_ctx.failures = 0;
    test_ctx.priv = NULL;

    /* Check filter */
    if (!kunit_name_matches_filter(test_ctx.name) &&
        !kunit_name_matches_filter(suite->name)) {
        return; /* skip */
    }

    g_total_tests_run++;

    /* Run suite-level setup */
    if (suite->setup)
        suite->setup(&test_ctx);

    if (kc->params) {
        /* Parameterized test: run once per parameter */
        for (int p = 0; kc->params[p].name != NULL; p++) {
            test_ctx.priv = kc->params[p].value;
            kprintf("[KUNIT] RUN:   %s.%s[%s]\n", suite->name,
                    test_ctx.name, kc->params[p].name);
            kc->run(&test_ctx);
        }
    } else {
        /* Normal test */
        kprintf("[KUNIT] RUN:   %s.%s\n", suite->name, test_ctx.name);
        kc->run(&test_ctx);
    }

    /* Run suite-level teardown */
    if (suite->teardown)
        suite->teardown(&test_ctx);

    /* Record result */
    if (test_ctx.status == KUNIT_SUCCESS) {
        g_total_tests_passed++;
        kprintf("[KUNIT] PASS:  %s.%s\n", suite->name, test_ctx.name);
    } else {
        g_total_tests_failed++;
        kprintf("[KUNIT] FAIL:  %s.%s (%d failures)\n",
                suite->name, test_ctx.name, test_ctx.failures);
    }
}

/* Run all test cases in a suite */
static void kunit_run_suite(struct kunit_suite *suite)
{
    if (!suite) return;

    kprintf("[KUNIT] SUITE: %s (%d cases)\n", suite->name,
            count_cases(suite));

    for (int i = 0; i < KUNIT_MAX_CASES; i++) {
        struct kunit_case *kc = &suite->cases[i];
        if (!kc->run)
            break; /* end of cases (NULL run) */
        kunit_run_case(suite, kc);
    }
}

/* Count the number of non-NULL cases in a suite */
static int count_cases(struct kunit_suite *suite)
{
    int count = 0;
    for (int i = 0; i < KUNIT_MAX_CASES; i++) {
        if (suite->cases[i].run)
            count++;
        else
            break;
    }
    return count;
}

/* ── Run all registered suites ─────────────────────────────────────── */

static void kunit_run_all(void)
{
    kunit_reset();
    kprintf("\n========================================\n");
    kprintf("[KUNIT] Running all %d test suites\n", g_suite_count);
    kprintf("========================================\n");

    for (int i = 0; i < g_suite_count; i++) {
        if (g_suites[i])
            kunit_run_suite(g_suites[i]);
    }

    kprintf("========================================\n");
    kprintf("[KUNIT] Results: %d passed, %d failed, "
            "%d assertions (%d failures)\n",
            g_total_tests_passed, g_total_tests_failed,
            g_total_assertions, g_total_assertion_fails);
    kprintf("========================================\n\n");
}

/* ── Results tracking ─────────────────────────────────────────────── */

int kunit_passed_count(void)
{
    return g_total_tests_passed;
}

int kunit_failed_count(void)
{
    return g_total_tests_failed;
}

int kunit_total_failures(void)
{
    return g_total_assertion_fails;
}

void kunit_reset(void)
{
    g_total_tests_run = 0;
    g_total_tests_passed = 0;
    g_total_tests_failed = 0;
    g_total_assertions = 0;
    g_total_assertion_fails = 0;
}

/* ── Debugfs callbacks ────────────────────────────────────────────── */

/* Read callback for /sys/kernel/debug/kunit/results */
static void kunit_results_read(char *buf, int *len)
{
    int pos = 0;
    int max = 1024;
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "KUnit Test Results\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "==================\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Suites:    %d\n", g_suite_count);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Run:       %d\n", g_total_tests_run);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Passed:    %d\n", g_total_tests_passed);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Failed:    %d\n", g_total_tests_failed);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Assertions: %d\n", g_total_assertions);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Assert Fails: %d\n", g_total_assertion_fails);
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "\nRegistered suites:\n");

    for (int i = 0; i < g_suite_count; i++) {
        if (g_suites[i]) {
            pos += snprintf(buf + pos, (size_t)(max - pos),
                            "  %2d: %s (%d cases)\n",
                            i, g_suites[i]->name,
                            count_cases(g_suites[i]));
        }
    }

    *len = (pos < max) ? pos : (max - 1);
    if (*len < 0) *len = 0;
}

/* Write callback for /sys/kernel/debug/kunit/run_all */
static int kunit_run_all_write(const char *buf, int len)
{
    /* Accept any content to trigger run */
    (void)buf;
    (void)len;
    kunit_run_all();
    return 0;
}

/* Read callback for /sys/kernel/debug/kunit/status */
static void kunit_status_read(char *buf, int *len)
{
    int pos = 0;
    pos += snprintf(buf + pos, 256,
                    "tests_run=%d passed=%d failed=%d "
                    "assertions=%d fails=%d suites=%d\n",
                    g_total_tests_run, g_total_tests_passed,
                    g_total_tests_failed,
                    g_total_assertions, g_total_assertion_fails,
                    g_suite_count);
    *len = pos;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void kunit_init(void)
{
    spinlock_init(&g_kunit_lock);
    kunit_reset();
    g_filter_active = 0;
    g_filter[0] = '\0';

    /* Register the built-in test suites */
    kunit_register_builtin_tests();

    /* Create debugfs entries under /sys/kernel/debug/kunit/ */
    debugfs_create_file("kunit/results",  kunit_results_read);
    debugfs_create_rw_file("kunit/run_all",
                           NULL,
                           kunit_run_all_write);
    debugfs_create_file("kunit/status", kunit_status_read);
    debugfs_create_rw_file("kunit/filter",
                           NULL,
                           kunit_filter_write);

    kprintf("[OK] KUnit: Kernel unit test framework initialized "
            "(%d suite slots, filter=%s)\n", KUNIT_MAX_SUITES,
            g_filter_active ? g_filter : "none");
}

/* ── Filter API ───────────────────────────────────────────────────── */

void kunit_set_filter(const char *pattern)
{
    spinlock_acquire(&g_kunit_lock);
    if (!pattern || !pattern[0]) {
        g_filter_active = 0;
        g_filter[0] = '\0';
    } else {
        strncpy(g_filter, pattern, sizeof(g_filter) - 1);
        g_filter[sizeof(g_filter) - 1] = '\0';
        g_filter_active = 1;
    }
    spinlock_release(&g_kunit_lock);
}

const char *kunit_get_filter(void)
{
    if (!g_filter_active)
        return NULL;
    return g_filter;
}

/* Write callback for /sys/kernel/debug/kunit/filter */
static int kunit_filter_write(const char *buf, int len)
{
    if (!buf || len <= 0) {
        kunit_set_filter(NULL);
        return 0;
    }

    /* Copy and null-terminate */
    char pattern[KUNIT_MAX_NAME];
    int copy_len = len < (int)sizeof(pattern) - 1 ? len : (int)sizeof(pattern) - 1;
    memcpy(pattern, buf, copy_len);
    pattern[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (pattern[copy_len - 1] == '\n' ||
           pattern[copy_len - 1] == '\r' || pattern[copy_len - 1] == ' '))
        pattern[--copy_len] = '\0';

    if (copy_len == 0) {
        kunit_set_filter(NULL);
        kprintf("[KUNIT] Filter cleared\n");
    } else {
        kunit_set_filter(pattern);
        kprintf("[KUNIT] Filter set to: '%s'\n", pattern);
    }

    return 0;
}

/* ── Stub: kunit_run_test ─────────────────────────────── */
int kunit_run_test(const char *suite, const char *test)
{
    (void)suite;
    (void)test;
    kprintf("[kunit] kunit_run_test: not yet implemented\n");
    return -ENOSYS;
}
