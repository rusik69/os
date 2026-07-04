#ifndef KUNIT_COVERAGE_H
#define KUNIT_COVERAGE_H

/*
 * kunit_coverage.h — KUnit gcov-style coverage tracking.
 *
 * Provides a lightweight in-kernel coverage tracking system that
 * records which functions / code paths are exercised during KUnit
 * test runs.  Modeled after gcov's function-coverage semantics:
 * each tracked point is recorded once per coverage session and
 * its hit count incremented each time the point is reached.
 *
 * Usage:
 *   void my_function(void) {
 *       KUNIT_COV_MARK();          // track this function
 *       if (rare_case) {
 *           KUNIT_COV_MARK_NAMED("rare_path");  // named point
 *       }
 *   }
 *
 * Debugfs interface (under /sys/kernel/debug/kunit/):
 *   coverage      - Read: show coverage report (text table)
 *                   Write "reset": clear all coverage counters
 */

#include "types.h"
#include "printf.h"

/* ── Constants ────────────────────────────────────────────────────── */

/** Maximum number of distinct coverage points tracked simultaneously. */
#define KUNIT_COV_MAX_ENTRIES  256

/** Maximum length of a coverage point name (including NUL). */
#define KUNIT_COV_NAME_LEN     48

/* ── Data structures ──────────────────────────────────────────────── */

/** A single tracked coverage point. */
struct kunit_cov_entry {
    char     name[KUNIT_COV_NAME_LEN];   /* function / point name */
    uint32_t hits;                        /* how many times hit */
    bool     in_use;                      /* slot active? */
};

/* ── Public API ───────────────────────────────────────────────────── */

/** Initialise the coverage tracking table.  Called once at boot. */
void kunit_coverage_init(void);

/**
 * Reset all coverage counters to zero, clearing all tracked points.
 * Typically called before each test run so coverage data reflects
 * only the code exercised by that run.
 */
void kunit_coverage_reset(void);

/**
 * Record a coverage hit for the named point / function.
 * If the point is already tracked, its counter is incremented.
 * Otherwise a new entry is created (up to KUNIT_COV_MAX_ENTRIES).
 *
 * @param name  Coverage point name (typically __func__ or a short string).
 * @return true  if recorded successfully,
 *         false if the table is full.
 */
bool kunit_coverage_hit(const char *name);

/**
 * Generate a human-readable coverage report into the given buffer.
 *
 * @param buf     Output buffer.
 * @param max_len Maximum number of bytes to write.
 * @return Number of bytes written (may be truncated).
 */
int kunit_coverage_report(char *buf, int max_len);

/**
 * Return the number of distinct coverage points that recorded at
 * least one hit (i.e. the number of active entries).
 */
int kunit_coverage_active_count(void);

/**
 * Return the total number of hits across all coverage points
 * (sum of all counters).
 */
uint32_t kunit_coverage_total_hits(void);

/* ── Marker macros ───────────────────────────────────────────────── */

/**
 * KUNIT_COV_MARK — Place at function entry to track coverage.
 *
 * Uses a static flag so each function / unique macro expansion is
 * registered exactly once per coverage session (gcov-style
 * "function executed" tracking).  Every subsequent call to the
 * same function increments the counter.
 *
 * Example:
 *   static void my_test_helper(struct kunit *test)
 *   {
 *       KUNIT_COV_MARK();
 *       // ... helper logic ...
 *   }
 */
#define KUNIT_COV_MARK() \
    do { \
        static bool __kcov_marked_ = false; \
        if (!__kcov_marked_) { \
            __kcov_marked_ = true; \
            kunit_coverage_hit(__func__); \
        } \
    } while (0)

/**
 * KUNIT_COV_MARK_NAMED — Record a hit for a named, ad-hoc coverage point.
 *
 * Unlike KUNIT_COV_MARK(), this does NOT use a static guard, so
 * every invocation increments the counter.  Use this for tracking
 * how often specific code paths (error branches, fallbacks) are
 * exercised.
 *
 * Example:
 *   if (error != 0) {
 *       KUNIT_COV_MARK_NAMED("error_path");
 *       return error;
 *   }
 */
#define KUNIT_COV_MARK_NAMED(name) \
    kunit_coverage_hit(name)

#endif /* KUNIT_COVERAGE_H */
