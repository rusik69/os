#ifndef KUNIT_H
#define KUNIT_H

#include "types.h"
#include "printf.h"

/* ── Stress level control ──────────────────────────────────────────── */

/**
 * enum kunit_stress_level — Stress (intensity) level for KUnit tests.
 *
 * Each test case can declare the maximum stress level at which it should
 * still be executed by setting the &stress_max field of %struct kunit_case.
 * The global stress level (set via debugfs or API) acts as a threshold:
 *
 *   light    — Only tests with stress_max == KUNIT_STRESS_LIGHT run.
 *              Quick smoke tests, 1 iteration, no heavy workloads.
 *   moderate — Tests with stress_max up to KUNIT_STRESS_MODERATE run.
 *              Standard test suite with nominal iteration count.
 *   heavy    — All tests run regardless of stress_max.
 *              Exhaustive tests, stress scenarios, many iterations.
 *
 * New test cases default to KUNIT_STRESS_MODERATE (suitable for normal
 * test runs).  Mark deliberately expensive or long-running tests with
 * KUNIT_STRESS_HEAVY so they are skipped during quick smoke-test cycles.
 */
enum kunit_stress_level {
	KUNIT_STRESS_LIGHT     = 0,   /* smoke tests, always run */
	KUNIT_STRESS_MODERATE  = 1,   /* standard test intensity (default) */
	KUNIT_STRESS_HEAVY     = 2,   /* exhaustive / stress tests */
};

/*
 * KUnit — lightweight in-kernel unit test framework.
 *
 * Inspired by Linux KUnit, this provides a simple way to define and run
 * unit tests inside the kernel via a debugfs interface at
 * /sys/kernel/debug/kunit/.
 *
 * Usage:
 *
 *   1. Define a test:
 *        static void my_test(struct kunit *test)
 *        {
 *            KUNIT_EXPECT_EQ(test, 1 + 1, 2);
 *            KUNIT_EXPECT_TRUE(test, 1 == 1);
 *            KUNIT_EXPECT_NE(test, 0, 1);
 *        }
 *
 *   2. Register the test:
 *        static struct kunit_case my_cases[] = {
 *            KUNIT_CASE(my_test),
 *            {}
 *        };
 *        static struct kunit_suite my_suite = {
 *            .name = "my_suite",
 *            .cases = my_cases,
 *        };
 *        kunit_register_suite(&my_suite);
 *
 *   3. Run from shell:
 *        # echo run_all > /sys/kernel/debug/kunit/control
 *        # echo 1 > /sys/kernel/debug/kunit/run_all
 *        # echo my_suite > /sys/kernel/debug/kunit/run_suite
 *        # cat /sys/kernel/debug/kunit/results
 *        # echo reset > /sys/kernel/debug/kunit/control
 *        # echo 2 > /sys/kernel/debug/kunit/verbose
 *        # echo 5 > /sys/kernel/debug/kunit/iterations
 *   4. Filter by suite/test name:
 *        # echo pmm > /sys/kernel/debug/kunit/filter
 *        # echo 1 > /sys/kernel/debug/kunit/run_all
 */

/* Maximum number of test suites */
#define KUNIT_MAX_SUITES 32

/* Maximum number of test cases per suite */
#define KUNIT_MAX_CASES  32

/* Maximum length of a test or suite name */
#define KUNIT_MAX_NAME   48

/* Test result status */
enum kunit_status {
    KUNIT_SUCCESS = 0,
    KUNIT_FAILURE = 1,
    KUNIT_SKIPPED = 2,
};

/* Per-test context passed to test functions */
struct kunit {
    const char       *name;       /* current test name */
    enum kunit_status status;     /* overall test status */
    int               failures;   /* assertion failure count */
    void             *priv;       /* private data (set by suite setup) */
};

/* A single test case */
struct kunit_case {
	const char *name;                       /* test name (NULL-terminated) */
	void      (*run)(struct kunit *test);   /* test function */
	struct kunit_param *params;             /* optional: parameter array (NULL-terminated) */
	enum kunit_stress_level stress_max;     /* max stress level at which this test runs */
};

/* A test suite — group of related tests with optional setup/teardown */
struct kunit_suite {
    const char          *name;              /* suite name */
    void (*setup)(struct kunit *test);      /* optional: called before each case */
    void (*teardown)(struct kunit *test);   /* optional: called after each case */
    struct kunit_case    cases[KUNIT_MAX_CASES];
};

/* ── Registration API ─────────────────────────────────────────────── */

/* Register a test suite.  Returns 0 on success, -1 if table is full. */
int kunit_register_suite(struct kunit_suite *suite);

/* Initialise the KUnit subsystem and create debugfs entries. */
void kunit_init(void);

/* Register the built-in test suites (defined in kunit_tests.c).
 * Called from kunit_init(). */
void kunit_register_builtin_tests(void);

/* ── Filtering API ─────────────────────────────────────────────────── */

/* Set a test filter pattern (simple substring match on suite.case name).
 * Only suites/cases whose name contains 'pattern' will run.
 * Pass "" or NULL to clear the filter (run all). */
void kunit_set_filter(const char *pattern);

/* Get the current filter pattern, or NULL if none. */
const char *kunit_get_filter(void);

/* ── Stress level control ──────────────────────────────────────────── */

/**
 * Set the global stress level threshold.
 *
 * When the global stress level is set to a given value, only test cases
 * whose %stress_max <= global_level will execute.  A test case with
 * stress_max == KUNIT_STRESS_HEAVY will be skipped unless the global
 * level is at least KUNIT_STRESS_HEAVY.
 *
 * @param level  One of KUNIT_STRESS_LIGHT, KUNIT_STRESS_MODERATE,
 *               KUNIT_STRESS_HEAVY.
 */
void kunit_set_stress_level(enum kunit_stress_level level);

/**
 * Get the current global stress level threshold.
 *
 * Returns the value most recently set via kunit_set_stress_level()
 * or written to the debugfs 'stress_level' file.  Default is
 * KUNIT_STRESS_MODERATE.
 */
enum kunit_stress_level kunit_get_stress_level(void);

/**
 * Return a human-readable name for the given stress level.
 *
 * Returns "light", "moderate", or "heavy".  For invalid values,
 * returns "unknown".
 */
const char *kunit_stress_level_name(enum kunit_stress_level level);

/* ── Parameterized test support ────────────────────────────────────── */

/* A parameter generator provides an array of values to run a test with.
 * The test function receives 'test->priv' pointing to one parameter. */
struct kunit_param {
    const char *name;       /* parameter name (for display) */
    void       *value;      /* parameter value */
};

/* Parameter array terminator */
#define KUNIT_PARAM_END()  { .name = NULL, .value = NULL }

/* Declare a parameterized test case.
 * The test function will be called once per parameter.
 * 'params' must be a NULL-terminated array of struct kunit_param.
 * Defaults to KUNIT_STRESS_MODERATE. */
#define KUNIT_CASE_PARAM(test_fn, params) \
    { .name = #test_fn, .run = test_fn, .params = params, .stress_max = KUNIT_STRESS_MODERATE }

/* ── Test result tracking ──────────────────────────────────────────── */

/* Return the number of suites that passed. */
int kunit_passed_count(void);

/* Return the number of suites that had failures. */
int kunit_failed_count(void);

/* Return the total number of assertion failures across all suites. */
int kunit_total_failures(void);

/* Reset all test results. */
void kunit_reset(void);

/* Export test results as JSON into the given buffer.
 * @buf: output buffer
 * @len: in — max buffer size, out — bytes written */
void kunit_export_json(char *buf, int *len);

/* ── Assertion macros ──────────────────────────────────────────────── */

/* Internal helper — records a failure with file/line info */
void kunit_do_fail(struct kunit *test, const char *file, int line,
                   const char *fmt, ...) __printf(4, 5);

/* Internal helper — records a success assertion */
void kunit_do_pass(struct kunit *test);

/* Unique variable name generation */
#define KUNIT_CONCAT(a, b)  a##b
#define KUNIT_MAKE_UNIQUE(prefix)  KUNIT_CONCAT(prefix, __LINE__)

#define KUNIT_EXPECT_TRUE(test, cond) do {                                 \
    if (!(cond)) {                                                         \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected true, but '" #cond "' is false");           \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_FALSE(test, cond) do {                                \
    if ((cond)) {                                                          \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected false, but '" #cond "' is true");           \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_EQ(test, left, right) do {                            \
    __auto_type KUNIT_MAKE_UNIQUE(__left) = (left);                        \
    __auto_type KUNIT_MAKE_UNIQUE(__right) = (right);                      \
    if (KUNIT_MAKE_UNIQUE(__left) != KUNIT_MAKE_UNIQUE(__right)) {         \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected equality: left=%lld right=%lld",           \
                      (long long)KUNIT_MAKE_UNIQUE(__left),                \
                      (long long)KUNIT_MAKE_UNIQUE(__right));              \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_NE(test, left, right) do {                            \
    __auto_type KUNIT_MAKE_UNIQUE(__left) = (left);                        \
    __auto_type KUNIT_MAKE_UNIQUE(__right) = (right);                      \
    if (KUNIT_MAKE_UNIQUE(__left) == KUNIT_MAKE_UNIQUE(__right)) {         \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected non-equality, but both=%lld",             \
                      (long long)KUNIT_MAKE_UNIQUE(__left));               \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_NULL(test, ptr) do {                                  \
    if ((ptr) != NULL) {                                                   \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected NULL, but ptr=%p", (void*)(ptr));          \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_NOT_NULL(test, ptr) do {                              \
    if ((ptr) == NULL) {                                                   \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected non-NULL, but ptr is NULL");                \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

#define KUNIT_EXPECT_STREQ(test, left, right) do {                         \
    const char *KUNIT_MAKE_UNIQUE(__l) = (left);                           \
    const char *KUNIT_MAKE_UNIQUE(__r) = (right);                          \
    if (KUNIT_MAKE_UNIQUE(__l) == NULL || KUNIT_MAKE_UNIQUE(__r) == NULL   \
        || __builtin_strcmp(KUNIT_MAKE_UNIQUE(__l), KUNIT_MAKE_UNIQUE(__r)) != 0) { \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected string equality: left=\"%s\" right=\"%s\"", \
                      KUNIT_MAKE_UNIQUE(__l) ? KUNIT_MAKE_UNIQUE(__l) : "(null)", \
                      KUNIT_MAKE_UNIQUE(__r) ? KUNIT_MAKE_UNIQUE(__r) : "(null)"); \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

/* Convenience macro to define a single-case suite quickly.
 * Defaults to KUNIT_STRESS_MODERATE. */
#define KUNIT_CASE(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_MODERATE }

/* Declare a test case that runs at all stress levels (smoke test). */
#define KUNIT_CASE_LIGHT(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_LIGHT }

/* Declare a test case that only runs during heavy (exhaustive) stress. */
#define KUNIT_CASE_HEAVY(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_HEAVY }

/* Declare a parameterized test case (default stress = moderate). */
#define KUNIT_CASE_PARAM_DEFAULT(test_fn, params) \
    { .name = #test_fn, .run = test_fn, .params = params, .stress_max = KUNIT_STRESS_MODERATE }

/* ── Auto-registration via linker section ────────────────────────── */

/* Declare a suite for automatic registration at boot.
 * Place this macro invocation after the suite definition in the same .c file.
 * The suite pointer is placed in the .kunit_test_suites linker section and
 * automatically registered during kunit_init().  This eliminates the need
 * for manual kunit_register_suite() calls.
 *
 * Usage:
 *   static struct kunit_suite my_suite = { ... };
 *   KUNIT_TEST_SUITE(my_suite);
 */
#define KUNIT_TEST_SUITE(suite) \
    static struct kunit_suite *__kunit_suite_ptr_##suite    \
    __attribute__((__used__, __section__(".kunit_test_suites"))) = &suite

/* Linker symbols for the .kunit_test_suites section boundaries.
 * kunit_register_section_suites() iterates between start and end
 * to register all suites placed in the section. */
extern struct kunit_suite *__kunit_suites_start[];
extern struct kunit_suite *__kunit_suites_end[];

#endif /* KUNIT_H */
