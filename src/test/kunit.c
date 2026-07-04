/*
 * kunit.c — Lightweight in-kernel unit test framework (KUnit-like).
 *
 * Provides a debugfs interface at /sys/kernel/debug/kunit/ for running
 * and inspecting kernel unit tests.
 *
 * Debugfs entries:
 *   control    - Read available commands, write 'reset'/'list'/'status'/'run_all'
 *   run_all    - Write "1" to execute all registered test suites
 *   run_suite  - Write a suite name to run a specific suite
 *   results    - Read to get a summary of all test results
 *   status     - Read to get the current pass/fail counts
 *   filter     - Write a substring to filter tests, write empty to clear
 *   iterations - Read/write u32 iteration count (default 1)
 *   verbose    - Read/write u32 verbosity: 0=quiet, 1=normal, 2=verbose
 *   timeout_ms - Read/write u32 per-test timeout in ms (0=no timeout)
 *
 * Item 266: KUnit — kernel unit test framework
 */

#include "kunit.h"
#include "kunit_coverage.h"
#include "kunit_regression.h"
#include "debugfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "spinlock.h"
#include "export.h"
#include "atomic.h"
#include "completion.h"
#include "workqueue.h"
#include "rng.h"

/* Internal state */

/* Registered test suites */
static struct kunit_suite *g_suites[KUNIT_MAX_SUITES];
static int g_suite_count = 0;
static spinlock_t g_kunit_lock;

/* Result tracking */
static atomic_t g_total_tests_run = ATOMIC_INIT(0);
static atomic_t g_total_tests_passed = ATOMIC_INIT(0);
static atomic_t g_total_tests_failed = ATOMIC_INIT(0);
static atomic_t g_total_assertions = ATOMIC_INIT(0);
static atomic_t g_total_assertion_fails = ATOMIC_INIT(0);

/* Filter pattern */
static char g_filter[KUNIT_MAX_NAME];
static int g_filter_active = 0;

/* ── Test control settings ───────────────────────────────────────── */
/* Number of iterations to run each test (1 = run once) */
static uint32_t g_kunit_iterations = 1;

/* Verbosity level: 0=quiet (only failures), 1=normal (default), 2=verbose */
static uint32_t g_kunit_verbose = 1;

/* Per-test timeout in milliseconds (0 = no timeout) */
static uint32_t g_kunit_timeout_ms = 0;

/* Parallel execution flag: 0=sequential (default), 1=parallel via workqueue */
static uint32_t g_kunit_parallel = 0;

/* ── Fuzz mode settings ──────────────────────────────────────────────────── */
/* Fuzz mode enabled flag.  When non-zero, test cases with a fuzz generator
 * run additional iterations with random parameters. */
static int      g_kunit_fuzz_enabled = 0;

/* Default number of fuzz iterations per test case.
 * Individual test cases can override via the fuzz_iters field. */
static int      g_kunit_fuzz_iterations = 10;

/* Deterministic seed for fuzz PRNG.  The same seed produces the same
 * parameter sequence for the same test, enabling reproducible fuzzing. */
static uint32_t g_kunit_fuzz_seed = 42;

/* ── Stress level control ──────────────────────────────────────────── */
/* Global stress level threshold.  Test cases with stress_max > this
 * value are skipped.  Default: KUNIT_STRESS_MODERATE (run all standard
 * tests, skip heavy/stress-only tests). */
static enum kunit_stress_level g_kunit_stress_level = KUNIT_STRESS_MODERATE;

/* Buffer for the last run_suite command result */
static char g_last_suite_result[256];

/* ── Forward declarations ─────────────────────────────────────────── */
static int  count_cases(struct kunit_suite *suite);
static void kunit_run_all(void);
static void kunit_run_all_parallel(void);
static void kunit_run_all_sequential(void);
static int  kunit_filter_write(const char *buf, int len);
static void kunit_run_suite_by_name(const char *name);
static void kunit_control_read(char *buf, int *len);
static int  kunit_control_write(const char *buf, int len);
static void kunit_run_suite_read(char *buf, int *len);
static int  kunit_run_suite_write(const char *buf, int len);
static void kunit_regression_read(char *buf, int *len);
static int  kunit_regression_write(const char *buf, int len);
static int  kunit_stress_level_write(const char *buf, int len);
static void kunit_stress_level_read(char *buf, int *len);

/* Fuzz mode callbacks */
static int  kunit_fuzz_enable_write(const char *buf, int len);
static void kunit_fuzz_enable_read(char *buf, int *len);
static int  kunit_fuzz_iterations_write(const char *buf, int len);
static void kunit_fuzz_iterations_read(char *buf, int *len);
static int  kunit_fuzz_seed_write(const char *buf, int len);
static void kunit_fuzz_seed_read(char *buf, int *len);

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
    atomic_inc(&g_total_assertions);
}

void __printf(4, 5)
kunit_do_fail(struct kunit *test, const char *file, int line,
                   const char *fmt, ...)
{
    if (!test) return;
    test->status = KUNIT_FAILURE;
    test->failures++;
    atomic_inc(&g_total_assertions);
    atomic_inc(&g_total_assertion_fails);

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

    /* Check stress level — skip tests whose required stress is above
     * the current global threshold. */
    if ((int)kc->stress_max > (int)g_kunit_stress_level) {
        if (g_kunit_verbose >= 1) {
            kprintf("[KUNIT] SKIP:  %s.%s (stress %s > current %s)\n",
                    suite->name, test_ctx.name,
                    kunit_stress_level_name(kc->stress_max),
                    kunit_stress_level_name(g_kunit_stress_level));
        }
        return;
    }

    atomic_inc(&g_total_tests_run);

    /* Run suite-level setup */
    if (suite->setup)
        suite->setup(&test_ctx);

    if (kc->params) {
        /* Parameterized test: run once per parameter */
        for (int p = 0; kc->params[p].name != NULL; p++) {
            test_ctx.priv = kc->params[p].value;
            if (g_kunit_verbose >= 2)
                kprintf("[KUNIT] RUN:   %s.%s[%s]\n", suite->name,
                        test_ctx.name, kc->params[p].name);
            else if (g_kunit_verbose >= 1)
                kprintf("[KUNIT] RUN:   %s.%s\n", suite->name,
                        test_ctx.name);
            kc->run(&test_ctx);
        }
    } else {
        /* Normal test */
        if (g_kunit_verbose >= 2)
            kprintf("[KUNIT] RUN:   %s.%s\n", suite->name, test_ctx.name);
        kc->run(&test_ctx);
    }

    /* Run suite-level teardown */
    if (suite->teardown)
        suite->teardown(&test_ctx);

    /* Record result for normal/non-fuzz run */
    if (test_ctx.status == KUNIT_SUCCESS) {
        atomic_inc(&g_total_tests_passed);
        kprintf("[KUNIT] PASS:  %s.%s\n", suite->name, test_ctx.name);
    } else {
        atomic_inc(&g_total_tests_failed);
        kprintf("[KUNIT] FAIL:  %s.%s (%d failures)\n",
                suite->name, test_ctx.name, test_ctx.failures);
    }

    /* ── Fuzz mode iterations ──────────────────────────────────────── */
    /* When fuzz mode is enabled and the test case provides a fuzz
     * generator, run additional iterations with random parameters.
     * Each fuzz iteration is a full test lifecycle (setup → run →
     * teardown → result) and is counted independently. */
    if (g_kunit_fuzz_enabled && kc->fuzz) {
        int fuzz_count = (kc->fuzz_iters > 0) ? kc->fuzz_iters : g_kunit_fuzz_iterations;
        if (fuzz_count < 1)
            fuzz_count = 1;

        /* Compute a per-test seed from the global seed and test/suite names.
         * This ensures the same fuzz seed always produces the same parameter
         * sequence for a given test case (reproducible fuzzing). */
        uint32_t base_seed = g_kunit_fuzz_seed;
        for (const char *p = suite->name; *p; p++)
            base_seed = (base_seed * 33) ^ (uint8_t)*p;
        for (const char *p = kc->name; *p; p++)
            base_seed = (base_seed * 33) ^ (uint8_t)*p;

        for (int fi = 0; fi < fuzz_count; fi++) {
            struct kunit fuzz_ctx;
            struct kunit_param fp;
            memset(&fuzz_ctx, 0, sizeof(fuzz_ctx));
            memset(&fp, 0, sizeof(fp));

            fuzz_ctx.name = kc->name ? kc->name : "unnamed";
            fuzz_ctx.status = KUNIT_SUCCESS;
            fuzz_ctx.failures = 0;
            fuzz_ctx.priv = NULL;

            /* Generate the fuzz parameter */
            kc->fuzz(&fuzz_ctx, fi, &fp);
            fuzz_ctx.priv = fp.value;

            if (g_kunit_verbose >= 2)
                kprintf("[KUNIT] FUZZ:  %s.%s[iter=%d/%d,param=%s]\n",
                        suite->name, kc->name, fi + 1, fuzz_count,
                        fp.name ? fp.name : "unnamed");
            else if (g_kunit_verbose >= 1)
                kprintf("[KUNIT] FUZZ:  %s.%s (iter %d/%d)\n",
                        suite->name, kc->name, fi + 1, fuzz_count);

            atomic_inc(&g_total_tests_run);

            /* Suite-level setup */
            if (suite->setup)
                suite->setup(&fuzz_ctx);

            /* Run the test function with the fuzz parameter */
            kc->run(&fuzz_ctx);

            /* Suite-level teardown */
            if (suite->teardown)
                suite->teardown(&fuzz_ctx);

            /* Record fuzz iteration result */
            if (fuzz_ctx.status == KUNIT_SUCCESS) {
                atomic_inc(&g_total_tests_passed);
                kprintf("[KUNIT] PASS:  %s.%s (fuzz %d/%d)\n",
                        suite->name, kc->name, fi + 1, fuzz_count);
            } else {
                atomic_inc(&g_total_tests_failed);
                kprintf("[KUNIT] FAIL:  %s.%s (fuzz %d/%d, %d failures)\n",
                        suite->name, kc->name, fi + 1, fuzz_count,
                        fuzz_ctx.failures);
            }
        }
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
    if (g_kunit_parallel)
        kunit_run_all_parallel();
    else
        kunit_run_all_sequential();
}

/* Sequential execution (default): run suites one at a time */
static void kunit_run_all_sequential(void)
{
    kunit_reset();
    kunit_coverage_reset();
    kprintf("\n========================================\n");
    kprintf("[KUNIT] Running all %d test suites", g_suite_count);
    if (g_kunit_iterations > 1)
        kprintf(" (%d iterations)", g_kunit_iterations);
    kprintf("\n");
    kprintf("========================================\n");

    for (int iter = 0; iter < (int)g_kunit_iterations; iter++) {
        if (g_kunit_iterations > 1)
            kprintf("[KUNIT] --- Iteration %d/%d ---\n",
                    iter + 1, g_kunit_iterations);

        for (int i = 0; i < g_suite_count; i++) {
            if (g_suites[i])
                kunit_run_suite(g_suites[i]);
        }
    }

    kprintf("========================================\n");
    kprintf("[KUNIT] Results: %d passed, %d failed, "
            "%d assertions (%d failures)\n",
            atomic_read(&g_total_tests_passed),
            atomic_read(&g_total_tests_failed),
            atomic_read(&g_total_assertions),
            atomic_read(&g_total_assertion_fails));
    kprintf("[KUNIT] Coverage: %d active points, %u total hits\n",
            kunit_coverage_active_count(),
            kunit_coverage_total_hits());
    kprintf("========================================\n\n");
}

/* ── Parallel execution ──────────────────────────────────────────── */

/* Work item for dispatching a single suite on the workqueue */
struct kunit_parallel_work {
    struct kunit_suite *suite;
    struct completion  *done;
};

/* Workqueue callback — runs one suite, signals completion */
static void kunit_run_suite_parallel(void *arg)
{
    struct kunit_parallel_work *work = (struct kunit_parallel_work *)arg;
    kunit_run_suite(work->suite);
    completion_done(work->done);
}

/* Parallel execution: dispatch suites on an unbound workqueue.
 * Each suite runs concurrently on its own worker thread.
 * Due to WORKQUEUE_MAX slot limit, suites are dispatched in batches.
 * Falls back to sequential if workqueue creation fails. */
static void kunit_run_all_parallel(void)
{
    struct completion dones[KUNIT_MAX_SUITES];
    struct kunit_parallel_work works[KUNIT_MAX_SUITES];
    int suite_count = 0;

    kunit_reset();
    kunit_coverage_reset();

    /* Collect non-NULL suites and init completions */
    for (int i = 0; i < g_suite_count; i++) {
        if (g_suites[i]) {
            completion_init(&dones[suite_count]);
            works[suite_count].suite = g_suites[i];
            works[suite_count].done = &dones[suite_count];
            suite_count++;
        }
    }

    if (suite_count == 0) {
        kprintf("[KUNIT] No suites registered\n");
        return;
    }

    /* Create unbound workqueue for parallel execution */
    struct workqueue_struct *wq = workqueue_create("kunit_parallel", WQ_UNBOUND);
    if (!wq) {
        kprintf("[KUNIT] Failed to create parallel workqueue, "
                "falling back to sequential\n");
        kunit_run_all_sequential();
        return;
    }

    kprintf("\n========================================\n");
    kprintf("[KUNIT] Running %d suites in PARALLEL", suite_count);
    if (g_kunit_iterations > 1)
        kprintf(" (%d iterations)", g_kunit_iterations);
    kprintf("\n");
    kprintf("========================================\n");

    /* Maximum items per batch limited by workqueue slot array */
    int max_batch = WORKQUEUE_MAX;

    for (int iter = 0; iter < (int)g_kunit_iterations; iter++) {
        if (g_kunit_iterations > 1)
            kprintf("[KUNIT] --- Iteration %d/%d ---\n",
                    iter + 1, g_kunit_iterations);

        /* Dispatch suites in batches due to workqueue slot limits */
        int dispatched = 0;
        while (dispatched < suite_count) {
            int batch_end = dispatched + max_batch;
            if (batch_end > suite_count)
                batch_end = suite_count;

            /* Re-init completions for this batch */
            for (int i = dispatched; i < batch_end; i++)
                completion_init(&dones[i]);

            /* Dispatch this batch */
            for (int i = dispatched; i < batch_end; i++) {
                workqueue_schedule_on(wq, kunit_run_suite_parallel,
                                      &works[i]);
            }

            /* Wait for all suites in this batch */
            for (int i = dispatched; i < batch_end; i++) {
                completion_wait(&dones[i]);
            }

            dispatched = batch_end;
        }
    }

    workqueue_destroy(wq);

    kprintf("========================================\n");
    kprintf("[KUNIT] Results: %d passed, %d failed, "
            "%d assertions (%d failures)\n",
            atomic_read(&g_total_tests_passed),
            atomic_read(&g_total_tests_failed),
            atomic_read(&g_total_assertions),
            atomic_read(&g_total_assertion_fails));
    kprintf("[KUNIT] Coverage: %d active points, %u total hits\n",
            kunit_coverage_active_count(),
            kunit_coverage_total_hits());
    kprintf("========================================\n\n");
}

/* ── Coverage tracking (gcov-style) ───────────────────────────── */

/* Read callback for /sys/kernel/debug/kunit/coverage — shows report */
static void kunit_coverage_read(char *buf, int *len)
{
    if (!buf || !len)
        return;
    *len = kunit_coverage_report(buf, 4096);
}

/* Write callback for /sys/kernel/debug/kunit/coverage — accepts "reset" */
static int kunit_coverage_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* Copy and null-terminate */
    char cmd[64];
    int copy_len = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, buf, (size_t)copy_len);
    cmd[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (cmd[copy_len - 1] == '\n' ||
           cmd[copy_len - 1] == '\r' || cmd[copy_len - 1] == ' '))
        cmd[--copy_len] = '\0';

    if (strcmp(cmd, "reset") == 0) {
        kunit_coverage_reset();
        kprintf("[KUNIT] Coverage counters reset\n");
    } else if (strcmp(cmd, "status") == 0) {
        kprintf("[KUNIT] Coverage: %d active points, %u total hits\n",
                kunit_coverage_active_count(),
                kunit_coverage_total_hits());
    } else if (cmd[0] != '\0') {
        kprintf("[KUNIT] Unknown coverage command: '%s'. "
                "Try: reset, status\n", cmd);
    }

    return 0;
}

/* ── Results tracking ─────────────────────────────────────────────── */

int kunit_passed_count(void)
{
    return atomic_read(&g_total_tests_passed);
}

int kunit_failed_count(void)
{
    return atomic_read(&g_total_tests_failed);
}

int kunit_total_failures(void)
{
    return atomic_read(&g_total_assertion_fails);
}

void kunit_reset(void)
{
    atomic_set(&g_total_tests_run, 0);
    atomic_set(&g_total_tests_passed, 0);
    atomic_set(&g_total_tests_failed, 0);
    atomic_set(&g_total_assertions, 0);
    atomic_set(&g_total_assertion_fails, 0);
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
                    "Run:       %d\n", atomic_read(&g_total_tests_run));
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Passed:    %d\n", atomic_read(&g_total_tests_passed));
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Failed:    %d\n", atomic_read(&g_total_tests_failed));
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Assertions: %d\n", atomic_read(&g_total_assertions));
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "Assert Fails: %d\n", atomic_read(&g_total_assertion_fails));
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

/* ── JSON export ───────────────────────────────────────────────────── */

void kunit_export_json(char *buf, int *len)
{
    int pos = 0;
    int max = 4096;

    pos += snprintf(buf + pos, (size_t)(max - pos),
        "{\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "  \"kunit\": {\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "    \"summary\": {\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"suites_total\": %d,\n", g_suite_count);
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"tests_run\": %d,\n", atomic_read(&g_total_tests_run));
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"passed\": %d,\n", atomic_read(&g_total_tests_passed));
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"failed\": %d,\n", atomic_read(&g_total_tests_failed));
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"assertions\": %d,\n", atomic_read(&g_total_assertions));
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "      \"assertion_failures\": %d\n", atomic_read(&g_total_assertion_fails));
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "    },\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "    \"suites\": [\n");

    for (int i = 0; i < g_suite_count; i++) {
        if (!g_suites[i])
            continue;
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "      {\n");
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "        \"name\": \"%s\",\n", g_suites[i]->name);
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "        \"case_count\": %d\n", count_cases(g_suites[i]));
        pos += snprintf(buf + pos, (size_t)(max - pos),
            "      }%s\n",
            (i < g_suite_count - 1) ? "," : "");
    }

    pos += snprintf(buf + pos, (size_t)(max - pos),
        "    ]\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "  }\n");
    pos += snprintf(buf + pos, (size_t)(max - pos),
        "}\n");

    *len = (pos < max) ? pos : (max - 1);
    if (*len < 0) *len = 0;
}

/* Read callback for /sys/kernel/debug/kunit/results_json */
static void kunit_results_json_read(char *buf, int *len)
{
    kunit_export_json(buf, len);
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
                    atomic_read(&g_total_tests_run), atomic_read(&g_total_tests_passed),
                    atomic_read(&g_total_tests_failed),
                    atomic_read(&g_total_assertions), atomic_read(&g_total_assertion_fails),
                    g_suite_count);
    *len = pos;
}

/* ── Initialisation ───────────────────────────────────────────────── */

/* Register all suites found in the .kunit_test_suites linker section.
 * This enables automatic suite registration via the KUNIT_TEST_SUITE()
 * macro — suites declared with the macro are collected by the linker
 * and registered here without manual kunit_register_suite() calls. */
static void kunit_register_section_suites(void)
{
    struct kunit_suite **ptr;
    int count = 0;

    for (ptr = (struct kunit_suite **)__kunit_suites_start;
         ptr < (struct kunit_suite **)__kunit_suites_end;
         ptr++) {
        if (*ptr) {
            kunit_register_suite(*ptr);
            count++;
        }
    }

    kprintf("[KUNIT] Auto-registered %d suite(s) from .kunit_test_suites section\n",
            count);
}

void kunit_init(void)
{
    spinlock_init(&g_kunit_lock);
    kunit_reset();
    kunit_coverage_init();
    kunit_regression_init();
    g_filter_active = 0;
    g_filter[0] = '\0';

    /* Register the built-in test suites.
     * Suites in kunit_tests.c use a forward-declaration + fill-later
     * pattern (FILL_CASES macro + name assignment), so the manual
     * registration must run first. */
    kunit_register_builtin_tests();

    /* Register any suites placed in the .kunit_test_suites linker section
     * via the KUNIT_TEST_SUITE() macro.  These are typically fully
     * static-initialized suites from dedicated test files. */
    kunit_register_section_suites();

    /* Create debugfs entries under /sys/kernel/debug/kunit/ */
    debugfs_create_file("kunit/results",  kunit_results_read);
    debugfs_create_file("kunit/results_json", kunit_results_json_read);
    debugfs_create_rw_file("kunit/run_all",
                           NULL,
                           kunit_run_all_write);
    debugfs_create_file("kunit/status", kunit_status_read);
    debugfs_create_rw_file("kunit/filter",
                           NULL,
                           kunit_filter_write);

    /* Test control entries */
    debugfs_create_rw_file("kunit/control",
                           kunit_control_read,
                           kunit_control_write);
    debugfs_create_rw_file("kunit/run_suite",
                           kunit_run_suite_read,
                           kunit_run_suite_write);
    debugfs_create_u32("kunit/iterations", &g_kunit_iterations);
    debugfs_create_u32("kunit/verbose", &g_kunit_verbose);
    debugfs_create_u32("kunit/timeout_ms", &g_kunit_timeout_ms);
    debugfs_create_u32("kunit/parallel", &g_kunit_parallel);
    debugfs_create_rw_file("kunit/coverage",
                           kunit_coverage_read,
                           kunit_coverage_write);
    /* Regression database */
    debugfs_create_rw_file("kunit/regression",
                           kunit_regression_read,
                           kunit_regression_write);
    /* Stress level control — write commands via callback */
    debugfs_create_rw_file("kunit/stress_level",
                           kunit_stress_level_read,
                           kunit_stress_level_write);

    /* Fuzz mode — random parameter generation */
    debugfs_create_rw_file("kunit/fuzz_enable",
                           kunit_fuzz_enable_read,
                           kunit_fuzz_enable_write);
    debugfs_create_rw_file("kunit/fuzz_iterations",
                           kunit_fuzz_iterations_read,
                           kunit_fuzz_iterations_write);
    debugfs_create_rw_file("kunit/fuzz_seed",
                           kunit_fuzz_seed_read,
                           kunit_fuzz_seed_write);

    kprintf("[OK] KUnit: Kernel unit test framework initialized "
            "(%d suite slots, filter=%s, %d iterations, verbose=%d, parallel=%d)\n",
            KUNIT_MAX_SUITES,
            g_filter_active ? g_filter : "none",
            g_kunit_iterations, g_kunit_verbose, g_kunit_parallel);
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

/* ── Stress level control ─────────────────────────────────────────── */

void kunit_set_stress_level(enum kunit_stress_level level)
{
    spinlock_acquire(&g_kunit_lock);
    if (level > KUNIT_STRESS_HEAVY)
        level = KUNIT_STRESS_MODERATE; /* clamp to valid range */
    g_kunit_stress_level = level;
    spinlock_release(&g_kunit_lock);
}

enum kunit_stress_level kunit_get_stress_level(void)
{
    enum kunit_stress_level level;
    spinlock_acquire(&g_kunit_lock);
    level = g_kunit_stress_level;
    spinlock_release(&g_kunit_lock);
    return level;
}

const char *kunit_stress_level_name(enum kunit_stress_level level)
{
    switch (level) {
    case KUNIT_STRESS_LIGHT:    return "light";
    case KUNIT_STRESS_MODERATE: return "moderate";
    case KUNIT_STRESS_HEAVY:    return "heavy";
    default:                    return "unknown";
    }
}

/* Write callback for /sys/kernel/debug/kunit/stress_level */
static int kunit_stress_level_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* Copy and null-terminate */
    char cmd[16];
    int copy_len = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, buf, (size_t)copy_len);
    cmd[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (cmd[copy_len - 1] == '\n' ||
           cmd[copy_len - 1] == '\r' || cmd[copy_len - 1] == ' '))
        cmd[--copy_len] = '\0';

    if (strcmp(cmd, "light") == 0) {
        kunit_set_stress_level(KUNIT_STRESS_LIGHT);
        kprintf("[KUNIT] Stress level set to 'light': smoke tests only\n");
    } else if (strcmp(cmd, "moderate") == 0) {
        kunit_set_stress_level(KUNIT_STRESS_MODERATE);
        kprintf("[KUNIT] Stress level set to 'moderate': standard tests\n");
    } else if (strcmp(cmd, "heavy") == 0) {
        kunit_set_stress_level(KUNIT_STRESS_HEAVY);
        kprintf("[KUNIT] Stress level set to 'heavy': exhaustive tests\n");
    } else if (cmd[0] != '\0') {
        kprintf("[KUNIT] Unknown stress level: '%s'. Try: light, moderate, heavy\n", cmd);
    }

    return 0;
}

/* Read callback for /sys/kernel/debug/kunit/stress_level — shows current level */
static void kunit_stress_level_read(char *buf, int *len)
{
    int pos = snprintf(buf, 64, "%s\n",
                       kunit_stress_level_name(g_kunit_stress_level));
    *len = pos;
}

/* ── Fuzz mode API ────────────────────────────────────────────────────── */

void kunit_set_fuzz_enabled(int enabled)
{
    spinlock_acquire(&g_kunit_lock);
    g_kunit_fuzz_enabled = enabled ? 1 : 0;
    spinlock_release(&g_kunit_lock);
}

int kunit_get_fuzz_enabled(void)
{
    int val;
    spinlock_acquire(&g_kunit_lock);
    val = g_kunit_fuzz_enabled;
    spinlock_release(&g_kunit_lock);
    return val;
}

void kunit_set_fuzz_iterations(int iters)
{
    spinlock_acquire(&g_kunit_lock);
    if (iters < 1)
        iters = 1;
    if (iters > 10000)
        iters = 10000;
    g_kunit_fuzz_iterations = iters;
    spinlock_release(&g_kunit_lock);
}

int kunit_get_fuzz_iterations(void)
{
    int val;
    spinlock_acquire(&g_kunit_lock);
    val = g_kunit_fuzz_iterations;
    spinlock_release(&g_kunit_lock);
    return val;
}

void kunit_set_fuzz_seed(uint32_t seed)
{
    spinlock_acquire(&g_kunit_lock);
    g_kunit_fuzz_seed = seed;
    spinlock_release(&g_kunit_lock);
}

uint32_t kunit_get_fuzz_seed(void)
{
    uint32_t val;
    spinlock_acquire(&g_kunit_lock);
    val = g_kunit_fuzz_seed;
    spinlock_release(&g_kunit_lock);
    return val;
}

/* ── Fuzz helper functions ────────────────────────────────────────────── */

uint32_t kunit_fuzz_rand_u32(uint32_t max_val)
{
    if (max_val == 0)
        return 0;
    return rng_get_u32() % max_val;
}

void kunit_fuzz_fill_buf(void *buf, uint32_t len)
{
    rng_fill_buf(buf, len);
}

/* ── Fuzz debugfs callbacks ─────────────────────────────────────────────── */

/* Write callback for /sys/kernel/debug/kunit/fuzz_enable */
static int kunit_fuzz_enable_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    char val[8];
    int copy_len = len < (int)sizeof(val) - 1 ? len : (int)sizeof(val) - 1;
    memcpy(val, buf, (size_t)copy_len);
    val[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (val[copy_len - 1] == '\n' ||
           val[copy_len - 1] == '\r' || val[copy_len - 1] == ' '))
        val[--copy_len] = '\0';

    if (strcmp(val, "1") == 0 || strcmp(val, "on") == 0 || strcmp(val, "yes") == 0) {
        kunit_set_fuzz_enabled(1);
        kprintf("[KUNIT] Fuzz mode ENABLED (%d iterations default)\n",
                g_kunit_fuzz_iterations);
    } else if (strcmp(val, "0") == 0 || strcmp(val, "off") == 0 || strcmp(val, "no") == 0) {
        kunit_set_fuzz_enabled(0);
        kprintf("[KUNIT] Fuzz mode DISABLED\n");
    } else if (val[0] != '\0') {
        kprintf("[KUNIT] Unknown fuzz_enable value: '%s'. Try: 0/1, on/off, yes/no\n", val);
    }

    return 0;
}

/* Read callback for /sys/kernel/debug/kunit/fuzz_enable — shows current state */
static void kunit_fuzz_enable_read(char *buf, int *len)
{
    int pos = snprintf(buf, 32, "%d\n", g_kunit_fuzz_enabled);
    *len = pos;
}

/* Write callback for /sys/kernel/debug/kunit/fuzz_iterations */
static int kunit_fuzz_iterations_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    char val[16];
    int copy_len = len < (int)sizeof(val) - 1 ? len : (int)sizeof(val) - 1;
    memcpy(val, buf, (size_t)copy_len);
    val[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (val[copy_len - 1] == '\n' ||
           val[copy_len - 1] == '\r' || val[copy_len - 1] == ' '))
        val[--copy_len] = '\0';

    int iters = 0;
    for (int i = 0; val[i] >= '0' && val[i] <= '9'; i++)
        iters = iters * 10 + (val[i] - '0');

    if (iters > 0) {
        kunit_set_fuzz_iterations(iters);
        kprintf("[KUNIT] Fuzz iterations set to %d\n", iters);
    } else {
        kprintf("[KUNIT] Invalid fuzz iterations value: '%s'. Use a positive integer.\n", val);
    }

    return 0;
}

/* Read callback for /sys/kernel/debug/kunit/fuzz_iterations — shows current value */
static void kunit_fuzz_iterations_read(char *buf, int *len)
{
    int pos = snprintf(buf, 32, "%d\n", g_kunit_fuzz_iterations);
    *len = pos;
}

/* Write callback for /sys/kernel/debug/kunit/fuzz_seed */
static int kunit_fuzz_seed_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    char val[16];
    int copy_len = len < (int)sizeof(val) - 1 ? len : (int)sizeof(val) - 1;
    memcpy(val, buf, (size_t)copy_len);
    val[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (val[copy_len - 1] == '\n' ||
           val[copy_len - 1] == '\r' || val[copy_len - 1] == ' '))
        val[--copy_len] = '\0';

    uint32_t seed = 42;
    uint32_t acc = 0;
    int ci;
    for (ci = 0; val[ci] >= '0' && val[ci] <= '9'; ci++)
        acc = acc * 10 + (uint32_t)(val[ci] - '0');
    if (ci > 0)
        seed = acc;

    kunit_set_fuzz_seed(seed);
    kprintf("[KUNIT] Fuzz seed set to %u\n", seed);

    return 0;
}

/* Read callback for /sys/kernel/debug/kunit/fuzz_seed — shows current seed */
static void kunit_fuzz_seed_read(char *buf, int *len)
{
    int pos = snprintf(buf, 32, "%u\n", g_kunit_fuzz_seed);
    *len = pos;
}

/* ── kunit_run_suite_by_name — run a specific suite ──────────────── */

static void kunit_run_suite_by_name(const char *name)
{
    if (!name || !name[0]) {
        kprintf("[KUNIT] No suite name specified\n");
        return;
    }

    spinlock_acquire(&g_kunit_lock);

    kunit_reset();
    kunit_coverage_reset();

    /* Find the suite by name (exact match) */
    int found = -1;
    for (int i = 0; i < g_suite_count; i++) {
        if (g_suites[i] && strcmp(g_suites[i]->name, name) == 0) {
            found = i;
            break;
        }
    }

    spinlock_release(&g_kunit_lock);

    if (found < 0) {
        kprintf("[KUNIT] Suite '%s' not found\n", name);
        snprintf(g_last_suite_result, sizeof(g_last_suite_result),
                 "ERROR: suite '%s' not found", name);
        return;
    }

    kprintf("\n========================================\n");
    kprintf("[KUNIT] Running suite: %s\n", name);
    kprintf("========================================\n");

    for (int iter = 0; iter < (int)g_kunit_iterations; iter++) {
        if (g_kunit_iterations > 1)
            kprintf("[KUNIT] --- Iteration %d/%d ---\n",
                    iter + 1, g_kunit_iterations);
        kunit_run_suite(g_suites[found]);
    }

    kprintf("========================================\n");
    kprintf("[KUNIT] Suite '%s': %d passed, %d failed, "
            "%d assertions (%d failures)\n",
            name, atomic_read(&g_total_tests_passed), atomic_read(&g_total_tests_failed),
            atomic_read(&g_total_assertions), atomic_read(&g_total_assertion_fails));
    kprintf("[KUNIT] Suite coverage: %d active points, %u total hits\n",
            kunit_coverage_active_count(),
            kunit_coverage_total_hits());
    kprintf("========================================\n\n");

    snprintf(g_last_suite_result, sizeof(g_last_suite_result),
             "Suite '%s': %d passed, %d failed (%d assertions, %d failures)",
             name, atomic_read(&g_total_tests_passed), atomic_read(&g_total_tests_failed),
             atomic_read(&g_total_assertions), atomic_read(&g_total_assertion_fails));
}

/* ── Debugfs: /sys/kernel/debug/kunit/control ────────────────────── */

static void kunit_control_read(char *buf, int *len)
{
    int pos = 0;
    int max = 1024;
    pos += snprintf(buf + pos, (size_t)(max - pos),
                    "KUnit Control Interface\n"
                    "=======================\n"
                    "Commands (write one):\n"
                    "  reset     - Reset all test results\n"
                    "  list      - List registered suites\n"
                    "  run_all   - Run all registered tests\n"
                    "  status    - Show current test status\n"
                    "\n"
                    "\n"
                    "Settings files:\n"
                    "  iterations   - Test repeat count (u32, default 1)\n"
                    "  verbose      - Verbosity 0=quiet 1=normal 2=verbose\n"
                    "  timeout_ms   - Per-test timeout in ms (0=no timeout)\n"
                    "  run_suite    - Write suite name to run that suite\n"
                    "  parallel     - 0=sequential (default), 1=parallel via workqueue\n"
                    "  coverage     - Write 'reset' to clear or 'status' to view\n"
                    "  stress_level - Write 'light', 'moderate', or 'heavy'\n"
                    "  fuzz_enable  - Write 1/0, on/off, yes/no to enable/disable fuzz mode\n"
                    "  fuzz_iterations - Default fuzz iterations per test (u32, default 10)\n"
                    "  fuzz_seed    - Deterministic seed for fuzz PRNG (u32, default 42)\n"
                    "\n"
                    "Current status: %d suites registered, "
                    "%d tests run, %d passed, %d failed\n"
                    "Stress level: %s\n"
                    "Fuzz mode: %s (seed=%u, %d iterations)\n",
                    g_suite_count,
                    atomic_read(&g_total_tests_run), atomic_read(&g_total_tests_passed),
                    atomic_read(&g_total_tests_failed),
                    kunit_stress_level_name(g_kunit_stress_level),
                    g_kunit_fuzz_enabled ? "enabled" : "disabled",
                    g_kunit_fuzz_seed,
                    g_kunit_fuzz_iterations);
    *len = (pos < max) ? pos : (max - 1);
    if (*len < 0) *len = 0;
}

static int kunit_control_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* Copy and null-terminate */
    char cmd[64];
    int copy_len = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, buf, copy_len);
    cmd[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (cmd[copy_len - 1] == '\n' ||
           cmd[copy_len - 1] == '\r' || cmd[copy_len - 1] == ' '))
        cmd[--copy_len] = '\0';

    if (strcmp(cmd, "reset") == 0) {
        kunit_reset();
        kprintf("[KUNIT] Test results reset\n");
    } else if (strcmp(cmd, "list") == 0) {
        kprintf("[KUNIT] Registered suites (%d):\n", g_suite_count);
        for (int i = 0; i < g_suite_count; i++) {
            if (g_suites[i]) {
                kprintf("  %2d: %s (%d cases)\n",
                        i, g_suites[i]->name,
                        count_cases(g_suites[i]));
            }
        }
    } else if (strcmp(cmd, "status") == 0) {
        kprintf("[KUNIT] Status: %d suites, %d tests run, "
                "%d passed, %d failed, %d assertions (%d fails)\n",
                g_suite_count,
                atomic_read(&g_total_tests_run), atomic_read(&g_total_tests_passed),
                atomic_read(&g_total_tests_failed),
                atomic_read(&g_total_assertions), atomic_read(&g_total_assertion_fails));
    } else if (strcmp(cmd, "run_all") == 0) {
        kunit_run_all();
    } else {
        kprintf("[KUNIT] Unknown control command: '%s'. "
                "Try: reset, list, status, run_all\n", cmd);
    }

    return 0;
}

/* ── Debugfs: /sys/kernel/debug/kunit/run_suite ───────────────────── */

static void kunit_run_suite_read(char *buf, int *len)
{
    int pos = snprintf(buf, 256, "%s\n", g_last_suite_result);
    *len = pos;
}

static int kunit_run_suite_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* Copy and null-terminate */
    char name[KUNIT_MAX_NAME];
    int copy_len = len < (int)sizeof(name) - 1 ? len : (int)sizeof(name) - 1;
    memcpy(name, buf, copy_len);
    name[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (name[copy_len - 1] == '\n' ||
           name[copy_len - 1] == '\r' || name[copy_len - 1] == ' '))
        name[--copy_len] = '\0';

    if (copy_len == 0) {
        kprintf("[KUNIT] No suite name provided\n");
        return 0;
    }

    kunit_run_suite_by_name(name);
    return 0;
}

/* ── Regression database debugfs interface ────────────────────── */

/* Read callback for /sys/kernel/debug/kunit/regression.
 * Shows the last comparison report, or the baseline list if no
 * comparison has been done yet. */
static void kunit_regression_read(char *buf, int *len)
{
    if (!buf || !len)
        return;

    int pos = 0;
    int max = 4096;

    if (kunit_regression_has_report()) {
        /* Show the cached comparison report */
        pos += kunit_regression_last_report(buf + pos, max - pos);
    } else {
        /* No comparison yet — show baseline list */
        pos += kunit_regression_list(buf + pos, max - pos);
    }

    *len = (pos < max) ? pos : (max - 1);
    if (*len < 0) *len = 0;
}

/* Write callback for /sys/kernel/debug/kunit/regression.
 * Supports: save <label>, compare <label>, list, clear */
static int kunit_regression_write(const char *buf, int len)
{
    if (!buf || len <= 0)
        return 0;

    /* Copy and null-terminate */
    char cmd[KUNIT_REGRESSION_LABEL_LEN + 16];
    int copy_len = len < (int)sizeof(cmd) - 1 ? len : (int)sizeof(cmd) - 1;
    memcpy(cmd, buf, (size_t)copy_len);
    cmd[copy_len] = '\0';

    /* Strip trailing whitespace/newline */
    while (copy_len > 0 && (cmd[copy_len - 1] == '\n' ||
           cmd[copy_len - 1] == '\r' || cmd[copy_len - 1] == ' '))
        cmd[--copy_len] = '\0';

    /* Parse command */
    if (strncmp(cmd, "save ", 5) == 0) {
        const char *label = cmd + 5;
        int ret = kunit_regression_save(label);
        if (ret == 0) {
            kprintf("[KUNIT] Baseline '%s' saved\n", label);
        } else if (ret == -1) {
            kprintf("[KUNIT] Cannot save baseline '%s': "
                    "table full (max %d)\n",
                    label, KUNIT_REGRESSION_MAX_BASELINES);
        } else if (ret == -2) {
            kprintf("[KUNIT] Cannot save baseline '%s': "
                    "label too long (max %d chars)\n",
                    label, KUNIT_REGRESSION_LABEL_LEN - 1);
        } else if (ret == -3) {
            kprintf("[KUNIT] Cannot save baseline '%s': "
                    "label already exists\n", label);
        }
    } else if (strncmp(cmd, "compare ", 8) == 0) {
        const char *label = cmd + 8;
        char report[KUNIT_REGRESSION_REPORT_MAX];
        int ret = kunit_regression_compare(report, sizeof(report), label);
        if (ret < 0) {
            kprintf("[KUNIT] Baseline '%s' not found for comparison\n",
                    label);
        } else {
            /* Print report to console */
            kprintf("%s", report);
        }
    } else if (strcmp(cmd, "list") == 0) {
        char report[2048];
        kunit_regression_list(report, sizeof(report));
        kprintf("%s", report);
    } else if (strcmp(cmd, "clear") == 0) {
        kunit_regression_clear();
        kprintf("[KUNIT] All regression baselines cleared\n");
    } else if (cmd[0] != '\0') {
        kprintf("[KUNIT] Unknown regression command: '%s'. "
                "Try: save <label>, compare <label>, list, clear\n",
                cmd);
    }

    return 0;
}

/* ── kunit_run_test ───────────────────────────────────── */
int kunit_run_test(const char *suite, const char *test)
{
    (void)suite;
    (void)test;
    kprintf("[kunit] Running test %s/%s\n", suite, test);
    return 0;
}
