#ifndef KUNIT_REGRESSION_H
#define KUNIT_REGRESSION_H

/*
 * kunit_regression.h — KUnit regression database (baseline comparison).
 *
 * Provides a lightweight baseline storage and regression detection system
 * that snapshots KUnit test results and compares them against previous
 * runs.  Modeled after gcov-style baseline comparison: each baseline
 * captures pass/fail counts, assertion statistics, and coverage metrics,
 * enabling automated regression detection between runs.
 *
 * Usage:
 *   # Save current results as a baseline named "v1.0"
 *   echo "save v1.0" > /sys/kernel/debug/kunit/regression
 *
 *   # Run tests again, then compare against the baseline
 *   echo "compare v1.0" > /sys/kernel/debug/kunit/regression
 *   cat /sys/kernel/debug/kunit/regression
 *
 *   # List all saved baselines
 *   echo "list" > /sys/kernel/debug/kunit/regression
 *
 *   # Clear all baselines
 *   echo "clear" > /sys/kernel/debug/kunit/regression
 *
 * D250 task 16: KUnit regression database (compare against baseline)
 */

#include "types.h"

/* ── Constants ────────────────────────────────────────────────────── */

/** Maximum number of stored baselines. */
#define KUNIT_REGRESSION_MAX_BASELINES  8

/** Maximum length of a baseline label (including NUL). */
#define KUNIT_REGRESSION_LABEL_LEN      32

/** Maximum output buffer size for the regression report. */
#define KUNIT_REGRESSION_REPORT_MAX     4096

/* ── Data structures ──────────────────────────────────────────────── */

/** A single snapshot of KUnit test results. */
struct kunit_regression_snapshot {
    int      tests_run;          /* total tests executed */
    int      tests_passed;       /* tests that passed */
    int      tests_failed;       /* tests that failed */
    int      assertions;         /* total assertion calls */
    int      assertion_failures; /* failed assertions */
    uint32_t coverage_hits;      /* total coverage point hits */
    int      coverage_points;    /* distinct coverage points hit */
};

/** A stored baseline entry with metadata. */
struct kunit_regression_baseline {
    struct kunit_regression_snapshot snap;
    char    label[KUNIT_REGRESSION_LABEL_LEN];
    bool    in_use;
};

/* ── Public API ───────────────────────────────────────────────────── */

/**
 * Initialise the regression database.  Called once during KUnit init.
 * Clears all stored baselines.
 */
void kunit_regression_init(void);

/**
 * Take a snapshot of the current KUnit test results.
 * Reads the atomic counters and coverage stats into the given struct.
 */
void kunit_regression_snapshot(struct kunit_regression_snapshot *snap);

/**
 * Save the current test results as a named baseline.
 *
 * @param label  Short name for this baseline (e.g. "v1.0", "golden").
 * @return 0 on success, -1 if the table is full, -2 if label is too long.
 */
int kunit_regression_save(const char *label);

/**
 * Compare the current test results against a named baseline and write
 * a human-readable regression report into the given buffer.
 *
 * @param buf     Output buffer.
 * @param max_len Maximum number of bytes to write.
 * @param label   Baseline label to compare against.
 * @return Number of bytes written, or -1 if the baseline is not found.
 */
int kunit_regression_compare(char *buf, int max_len, const char *label);

/**
 * List all stored baselines into the given buffer.
 *
 * @param buf     Output buffer.
 * @param max_len Maximum number of bytes to write.
 * @return Number of bytes written.
 */
int kunit_regression_list(char *buf, int max_len);

/**
 * Clear all stored baselines.
 */
void kunit_regression_clear(void);

/**
 * Return the number of stored baselines.
 */
int kunit_regression_count(void);

/**
 * Copy the last comparison report into the given buffer.
 *
 * @param buf     Output buffer.
 * @param max_len Maximum number of bytes to copy.
 * @return Number of bytes written, or 0 if no comparison has been done.
 */
int kunit_regression_last_report(char *buf, int max_len);

/**
 * Return true if a comparison has been performed since init/clear.
 */
bool kunit_regression_has_report(void);

#endif /* KUNIT_REGRESSION_H */
