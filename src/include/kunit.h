#ifndef KUNIT_H
#define KUNIT_H

/**
 * DOC: KUnit — Lightweight In-Kernel Unit Test Framework
 *
 * Inspired by Linux KUnit, this provides a simple way to define and run
 * unit tests inside the OS kernel via a debugfs interface at
 * /sys/kernel/debug/kunit/.  The framework supports suite grouping,
 * setup/teardown, parameterized tests, fuzz testing, stress level
 * control, coverage tracking, regression detection, parallel execution,
 * timeout enforcement, and JSON result export.
 *
 * USAGE QUICK-START
 * =================
 *
 * 1. DEFINE A TEST CASE:
 *
 *        static void my_test(struct kunit *test)
 *        {
 *            KUNIT_EXPECT_EQ(test, 1 + 1, 2);
 *            KUNIT_EXPECT_TRUE(test, 1 == 1);
 *            KUNIT_EXPECT_NE(test, 0, 1);
 *        }
 *
 * 2. GROUP INTO A SUITE:
 *
 *        static struct kunit_case my_cases[] = {
 *            KUNIT_CASE(my_test),
 *            {}
 *        };
 *        static struct kunit_suite my_suite = {
 *            .name = "my_suite",
 *            .cases = my_cases,
 *        };
 *
 * 3. REGISTER (choose one method):
 *
 *    a) Automatic registration via linker section (preferred):
 *        KUNIT_TEST_SUITE(my_suite);
 *        // Suite is auto-registered during kunit_init().
 *
 *    b) Manual registration:
 *        kunit_register_suite(&my_suite);
 *        // Call from kunit_register_builtin_tests().
 *
 * 4. RUN FROM SHELL (via debugfs):
 *
 *        # echo run_all > /sys/kernel/debug/kunit/control
 *        # echo 1 > /sys/kernel/debug/kunit/run_all
 *        # echo my_suite > /sys/kernel/debug/kunit/run_suite
 *        # cat /sys/kernel/debug/kunit/results
 *        # echo reset > /sys/kernel/debug/kunit/control
 *        # echo 2 > /sys/kernel/debug/kunit/verbose
 *        # echo 5 > /sys/kernel/debug/kunit/iterations
 *
 * 5. FILTER BY SUITE/TEST NAME:
 *
 *        # echo pmm > /sys/kernel/debug/kunit/filter
 *        # echo 1 > /sys/kernel/debug/kunit/run_all
 *
 * DEBUGFS INTERFACE
 * =================
 *   control      - Read available commands; write 'reset'/'list'/'status'/'run_all'
 *   run_all      - Write "1" to execute all registered test suites
 *   run_suite    - Write a suite name to run a specific suite
 *   results      - Read to get a summary of all test results
 *   status       - Read to get the current pass/fail counts
 *   filter       - Write a substring to filter tests; write empty to clear
 *   iterations   - Read/write u32 iteration count (default 1)
 *   verbose      - Read/write u32 verbosity: 0=quiet, 1=normal, 2=verbose
 *   timeout_ms   - Read/write u32 per-test timeout in ms (0=no timeout)
 *   coverage     - Read: show gcov-style coverage report
 *   regression   - Read/write: save/compare/list baselines
 *   stress_level - Read/write: light/moderate/heavy
 *   fuzz_enabled - Read/write: enable/disable fuzz mode
 *   fuzz_iters   - Read/write: default fuzz iterations
 *   fuzz_seed    - Read/write: deterministic fuzz PRNG seed
 *   log          - Read: show ring-buffer log; write: clear
 *   log_size     - Read/write: configure log buffer size (256-65536)
 *
 * KUNIT VS LEGACY ASSERT TESTS
 * =============================
 * The test.h / test.c framework (compiled with -DTEST_MODE) provides
 * simple ASSERT / ASSERT_EQ / ASSERT_STR macros for quick smoke tests.
 * KUnit provides richer features (suites, parameterization, fuzzing,
 * coverage, regression, JSON export, debugfs control).  Both can
 * coexist in the same kernel build.
 */

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

/**
 * KUNIT_MAX_SUITES — Maximum number of test suites that can be registered.
 *
 * The suite table is a fixed-size static array.  Attempting to register
 * more than KUNIT_MAX_SUITES suites returns -1 from kunit_register_suite().
 */
#define KUNIT_MAX_SUITES 32

/**
 * KUNIT_MAX_CASES — Maximum number of test cases per suite.
 *
 * The &struct kunit_suite contains a fixed-size array of cases.
 * Terminate the array with a case whose .run field is NULL.
 */
#define KUNIT_MAX_CASES  32

/**
 * KUNIT_MAX_NAME — Maximum length of a test or suite name (including NUL).
 *
 * Names longer than this are truncated by the storage structures.
 */
#define KUNIT_MAX_NAME   48

/**
 * enum kunit_status — Test result status.
 *
 * @KUNIT_SUCCESS:  All assertions passed.
 * @KUNIT_FAILURE:  One or more assertions failed.
 * @KUNIT_SKIPPED:  Test was skipped (e.g. stress level too high).
 */
enum kunit_status {
    KUNIT_SUCCESS = 0,
    KUNIT_FAILURE = 1,
    KUNIT_SKIPPED = 2,
};

/**
 * struct kunit — Per-test context passed to test functions.
 *
 * @name:      Pointer to the test case name string.
 * @status:    Overall test status (KUNIT_SUCCESS or KUNIT_FAILURE).
 * @failures:  Number of assertion failures recorded during this test.
 * @priv:      Private data pointer, set by suite setup or parameter
 *             framework.  Test functions read this to obtain per-parameter
 *             or per-invocation context.
 */
struct kunit {
    const char       *name;       /* current test name */
    enum kunit_status status;     /* overall test status */
    int               failures;   /* assertion failure count */
    void             *priv;       /* private data (set by suite setup) */
};

/**
 * struct kunit_param — A single parameter value for parameterized tests.
 *
 * @name:  Human-readable parameter name (for debug output).
 * @value: Opaque pointer to the parameter data.  Must remain valid
 *         for the duration of the test function call.
 *
 * Parameterized tests declare an array of these, terminated by
 * KUNIT_PARAM_END().  Each parameter value is passed to the test
 * function via test->priv.
 */
struct kunit_param {
    const char *name;       /* parameter name (for display) */
    void       *value;      /* parameter value */
};

/**
 * typedef kunit_fuzz_fn_t — Fuzz parameter generator callback.
 *
 * @test:        The current test context.
 * @iteration:   Zero-based iteration number (0, 1, 2, ...).
 * @out_param:   Output: the callback fills this with a name and value.
 *
 * Called for each fuzz iteration to produce a random parameter value.
 * The value stored in @out_param must remain valid for the duration
 * of the test function call.  Test authors may use rng_get_u32(),
 * rng_fill_buf(), or the iteration number for deterministic fuzzing.
 */
typedef void (*kunit_fuzz_fn_t)(struct kunit *test, int iteration,
                                struct kunit_param *out_param);

/**
 * struct kunit_case — A single test case definition.
 *
 * @name:        Human-readable test name (NULL-terminated).  Shown in
 *               debugfs output and used for filtering.
 * @run:         Pointer to the test function.  Called with a &struct
 *               kunit context.  The test function uses KUNIT_EXPECT_*
 *               macros to make assertions.
 * @params:      Optional pointer to a NULL-terminated array of
 *               %struct kunit_param for parameterized testing.  If set,
 *               the test function is called once per parameter entry.
 * @stress_max:  Maximum stress level at which this test should execute.
 *               Tests with stress_max &gt; the global threshold are skipped.
 * @fuzz:        Optional fuzz parameter generator callback.  When fuzz
 *               mode is enabled, the test runs additional iterations
 *               with parameters produced by this generator.
 * @fuzz_iters:  Per-test fuzz iteration count override.  If 0, the
 *               global default (g_kunit_fuzz_iterations) is used.
 *
 * Terminate a case array with a case whose .run = NULL.
 */
struct kunit_case {
	const char *name;                       /* test name (NULL-terminated) */
	void      (*run)(struct kunit *test);   /* test function */
	struct kunit_param *params;             /* optional: parameter array (NULL-terminated) */
	enum kunit_stress_level stress_max;     /* max stress level at which this test runs */
	kunit_fuzz_fn_t fuzz;                   /* optional: fuzz parameter generator */
	int             fuzz_iters;             /* per-test fuzz iterations (0 = use global default) */
};

/**
 * struct kunit_suite — A group of related test cases with optional
 *                      setup and teardown.
 *
 * @name:     Unique suite name.  Used for debugfs output and filtering.
 * @setup:    Optional callback invoked before each test case in the
 *            suite.  Receives the per-test &struct kunit context.
 *            Typically used to initialize shared state accessible via
 *            test->priv.
 * @teardown: Optional callback invoked after each test case.  Receives
 *            the same &struct kunit context that was passed to setup
 *            and run.  Used for cleanup.
 * @cases:    Fixed-size array of test cases.  Terminate with a case
 *            whose .run is NULL.
 */
struct kunit_suite {
    const char          *name;              /* suite name */
    void (*setup)(struct kunit *test);      /* optional: called before each case */
    void (*teardown)(struct kunit *test);   /* optional: called after each case */
    struct kunit_case    cases[KUNIT_MAX_CASES];
};

/* ── Parameter array terminator ──────────────────────────────────── */

/**
 * KUNIT_PARAM_END() — Terminator for parameterized test parameter arrays.
 *
 * Usage:
 *   static struct kunit_param my_params[] = {
 *       { .name = "small",  .value = &small_value },
 *       { .name = "large",  .value = &large_value },
 *       KUNIT_PARAM_END()
 *   };
 */
#define KUNIT_PARAM_END()  { .name = NULL, .value = NULL }

/* ── Registration API ─────────────────────────────────────────────── */

/**
 * kunit_register_suite() — Register a test suite.
 *
 * @suite: Pointer to a statically-defined &struct kunit_suite.
 * @return: 0 on success, -1 if the suite table is full or the
 *          suite name is a duplicate.
 *
 * Registering a suite makes it available for execution via the
 * debugfs interface.  Suites may also be registered automatically
 * via the KUNIT_TEST_SUITE() macro and the .kunit_test_suites
 * linker section.
 */
int kunit_register_suite(struct kunit_suite *suite);

/**
 * kunit_init() — Initialise the KUnit subsystem.
 *
 * Creates all debugfs entries under /sys/kernel/debug/kunit/ and
 * registers built-in test suites.  Called once during kernel boot
 * (from kernel_main).
 */
void kunit_init(void);

/**
 * kunit_register_builtin_tests() — Register the built-in test suites.
 *
 * Called from kunit_init().  Adds all KUnit test suites defined in
 * src/test/kunit_tests.c and related per-subsystem files.  New
 * suites added to the build system should be registered here.
 */
void kunit_register_builtin_tests(void);

/* ── Filtering API ─────────────────────────────────────────────────── */

/**
 * kunit_set_filter() — Set a test filter pattern.
 *
 * @pattern: Substring to match against suite.case names.  Pass ""
 *           or NULL to clear the filter and run all tests.
 *
 * Only suites/cases whose name contains the pattern will be
 * executed.  Matching is case-sensitive substring.
 */
void kunit_set_filter(const char *pattern);

/**
 * kunit_get_filter() — Get the current filter pattern.
 *
 * @return: Pointer to the current filter string, or NULL if none.
 */
const char *kunit_get_filter(void);

/* ── Stress level control ──────────────────────────────────────────── */

/**
 * kunit_set_stress_level() — Set the global stress level threshold.
 *
 * @level: One of KUNIT_STRESS_LIGHT, KUNIT_STRESS_MODERATE,
 *         KUNIT_STRESS_HEAVY.
 *
 * When the global stress level is set to a given value, only test cases
 * whose stress_max &lt;= global_level will execute.  A test case with
 * stress_max == KUNIT_STRESS_HEAVY will be skipped unless the global
 * level is at least KUNIT_STRESS_HEAVY.
 */
void kunit_set_stress_level(enum kunit_stress_level level);

/**
 * kunit_get_stress_level() — Get the current global stress level threshold.
 *
 * @return: The value most recently set via kunit_set_stress_level()
 *          or written to the debugfs 'stress_level' file.  Default is
 *          KUNIT_STRESS_MODERATE.
 */
enum kunit_stress_level kunit_get_stress_level(void);

/**
 * kunit_stress_level_name() — Return a human-readable name for a
 *                              stress level.
 *
 * @level: The stress level to convert.
 * @return: "light", "moderate", or "heavy".  For invalid values,
 *          returns "unknown".
 */
const char *kunit_stress_level_name(enum kunit_stress_level level);

/* ── Parameterized test support ────────────────────────────────────── */

/**
 * KUNIT_CASE_PARAM() — Declare a parameterized test case.
 *
 * @test_fn:  Test function (takes a single struct kunit * argument).
 * @params:   NULL-terminated array of struct kunit_param.
 *
 * The test function will be called once per parameter, with each
 * parameter value accessible via test->priv.  Defaults to
 * KUNIT_STRESS_MODERATE.
 */
#define KUNIT_CASE_PARAM(test_fn, params) \
    { .name = #test_fn, .run = test_fn, .params = params, .stress_max = KUNIT_STRESS_MODERATE }

/* ── Test result tracking ──────────────────────────────────────────── */

/**
 * kunit_passed_count() — Return the number of suites that passed.
 * @return: Number of suites with zero failures.
 */
int kunit_passed_count(void);

/**
 * kunit_failed_count() — Return the number of suites that had failures.
 * @return: Number of suites with one or more failures.
 */
int kunit_failed_count(void);

/**
 * kunit_total_failures() — Return the total number of assertion failures.
 * @return: Sum of failures across all suites.
 */
int kunit_total_failures(void);

/**
 * kunit_reset() — Reset all test result counters.
 *
 * Clears pass/fail/assertion counts, coverage data, and log buffer.
 * Typically called before starting a new test run.
 */
void kunit_reset(void);

/**
 * kunit_export_json() — Export test results as JSON.
 *
 * @buf: Output buffer.  Must be pre-allocated by the caller.
 * @len: On input — maximum buffer size in bytes; on output — number
 *       of bytes written (may be truncated).
 *
 * Produces a JSON object with suite names, test names, and their
 * pass/fail statuses.  Useful for CI integration.
 */
void kunit_export_json(char *buf, int *len);

/* ── Fuzz mode API ────────────────────────────────────────────────── */

/**
 * kunit_set_fuzz_enabled() — Enable or disable fuzz mode.
 *
 * @enabled: Non-zero to enable, zero to disable.
 *
 * When enabled, test cases with a fuzz generator run additional
 * iterations with randomly generated parameters.
 */
void kunit_set_fuzz_enabled(int enabled);

/**
 * kunit_get_fuzz_enabled() — Returns non-zero if fuzz mode is enabled.
 */
int kunit_get_fuzz_enabled(void);

/**
 * kunit_set_fuzz_iterations() — Set the default number of fuzz
 *                                iterations per test case.
 *
 * @iters:  Number of fuzz iterations.  Individual test cases can
 *          override via the fuzz_iters field of &struct kunit_case.
 */
void kunit_set_fuzz_iterations(int iters);

/**
 * kunit_get_fuzz_iterations() — Get the current default fuzz
 *                                iteration count.
 */
int kunit_get_fuzz_iterations(void);

/**
 * kunit_set_fuzz_seed() — Set the fuzz deterministic seed.
 *
 * @seed:  PRNG seed value.
 *
 * The seed is mixed with the suite/test name and iteration number
 * to produce deterministic parameter sequences.  Using the same
 * seed reproduces the same fuzz parameter sequence across runs.
 */
void kunit_set_fuzz_seed(uint32_t seed);

/**
 * kunit_get_fuzz_seed() — Get the current fuzz PRNG seed.
 */
uint32_t kunit_get_fuzz_seed(void);

/* ── Fuzz helper functions (available in fuzz callbacks) ──────────── */

/**
 * kunit_fuzz_rand_u32() — Return a random u32 in [0, max_val).
 *
 * @max_val:  Upper bound (exclusive).  Must be &gt; 0.
 * @return:   A uniformly distributed random value in [0, max_val).
 *
 * Uses the kernel RNG ("rng_get_u32").  Results differ between kernel
 * runs.  For reproducible fuzzing, generate values deterministically
 * from the iteration number instead.
 */
uint32_t kunit_fuzz_rand_u32(uint32_t max_val);

/**
 * kunit_fuzz_fill_buf() — Fill a buffer with random bytes.
 *
 * @buf:  Buffer to fill.
 * @len:  Number of bytes to write.
 *
 * Uses the kernel RNG.  For reproducible fuzzing, use deterministic
 * values derived from the iteration number and seed.
 */
void kunit_fuzz_fill_buf(void *buf, uint32_t len);

/* ── Assertion macros ──────────────────────────────────────────────── */

/**
 * kunit_do_fail() — Internal helper to record an assertion failure.
 *
 * @test:  The current test context (may be NULL).
 * @file:  Source file name (typically __FILE__).
 * @line:  Source line number (typically __LINE__).
 * @fmt:   printf-style format string.
 * @...:   Format arguments.
 *
 * Sets test->status to KUNIT_FAILURE, increments test->failures,
 * and prints a formatted failure message.  Called by the KUNIT_EXPECT_*
 * macros.
 */
void kunit_do_fail(struct kunit *test, const char *file, int line,
                   const char *fmt, ...) __printf(4, 5);

/**
 * kunit_do_pass() — Internal helper to record a successful assertion.
 *
 * @test:  The current test context.
 *
 * Increments the global assertion counter.  Called by the
 * KUNIT_EXPECT_* macros when the assertion passes.
 */
void kunit_do_pass(struct kunit *test);

/* ── Internal concatenation helpers ─────────────────────────────────── */

/** @cond INTERNAL */
#define KUNIT_CONCAT(a, b)  a##b
#define KUNIT_MAKE_UNIQUE(prefix)  KUNIT_CONCAT(prefix, __LINE__)
/** @endcond */

/**
 * KUNIT_EXPECT_TRUE() — Assert that a condition is true.
 *
 * @test:  The current test context (&struct kunit *).
 * @cond:  Boolean expression to evaluate.
 *
 * If @cond evaluates to false, the test is marked as failed and a
 * diagnostic message with the source location and expression text
 * is emitted.
 */
#define KUNIT_EXPECT_TRUE(test, cond) do {                                 \
    if (!(cond)) {                                                         \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected true, but '" #cond "' is false");           \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

/**
 * KUNIT_EXPECT_FALSE() — Assert that a condition is false.
 *
 * @test:  The current test context (&struct kunit *).
 * @cond:  Boolean expression to evaluate.
 *
 * If @cond evaluates to true, the test is marked as failed and a
 * diagnostic message is emitted.
 */
#define KUNIT_EXPECT_FALSE(test, cond) do {                                \
    if ((cond)) {                                                          \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected false, but '" #cond "' is true");           \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

/**
 * KUNIT_EXPECT_EQ() — Assert that two values are equal.
 *
 * @test:   The current test context (&struct kunit *).
 * @left:   Left-hand (actual) value.
 * @right:  Right-hand (expected) value.
 *
 * Uses __auto_type to evaluate each argument exactly once and
 * preserve type information.  On failure, both values are printed.
 * Works with any type that supports != comparison.
 */
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

/**
 * KUNIT_EXPECT_NE() — Assert that two values are NOT equal.
 *
 * @test:   The current test context (&struct kunit *).
 * @left:   First value.
 * @right:  Second value.
 *
 * On failure, the common value is printed.  Works with any type
 * that supports == comparison.
 */
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

/**
 * KUNIT_EXPECT_NULL() — Assert that a pointer is NULL.
 *
 * @test:  The current test context (&struct kunit *).
 * @ptr:   Pointer expression to test.
 */
#define KUNIT_EXPECT_NULL(test, ptr) do {                                  \
    if ((ptr) != NULL) {                                                   \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected NULL, but ptr=%p", (void*)(ptr));          \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

/**
 * KUNIT_EXPECT_NOT_NULL() — Assert that a pointer is NOT NULL.
 *
 * @test:  The current test context (&struct kunit *).
 * @ptr:   Pointer expression to test.
 */
#define KUNIT_EXPECT_NOT_NULL(test, ptr) do {                              \
    if ((ptr) == NULL) {                                                   \
        kunit_do_fail(test, __FILE__, __LINE__,                             \
                      "Expected non-NULL, but ptr is NULL");                \
    } else {                                                               \
        kunit_do_pass(test);                                                \
    }                                                                      \
} while (0)

/**
 * KUNIT_EXPECT_STREQ() — Assert that two C strings are equal.
 *
 * @test:  The current test context (&struct kunit *).
 * @left:  First string (may be NULL).
 * @right:  Second string (may be NULL).
 *
 * Compares using __builtin_strcmp().  NULL-safe: if either string
 * is NULL, the assertion fails with a clear message.  On failure,
 * both strings are printed (or "(null)" if NULL).
 */
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

/* ── Case declaration convenience macros ──────────────────────────── */

/**
 * KUNIT_CASE() — Declare a test case with default (moderate) stress.
 *
 * @test_fn:  Test function (takes a single struct kunit * argument).
 *
 * Macro-expands to a &struct kunit_case initializer with
 * stress_max = KUNIT_STRESS_MODERATE.
 */
#define KUNIT_CASE(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_MODERATE }

/**
 * KUNIT_CASE_LIGHT() — Declare a test case that runs at all stress
 *                       levels (smoke test).
 *
 * @test_fn:  Test function.
 *
 * Equivalent to KUNIT_CASE() but with stress_max = KUNIT_STRESS_LIGHT,
 * ensuring the test always runs regardless of the global stress level.
 */
#define KUNIT_CASE_LIGHT(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_LIGHT }

/**
 * KUNIT_CASE_HEAVY() — Declare a test case that only runs during
 *                       heavy (exhaustive) stress testing.
 *
 * @test_fn:  Test function.
 *
 * Equivalent to KUNIT_CASE() but with stress_max = KUNIT_STRESS_HEAVY.
 * Suitable for long-running or resource-intensive tests that should
 * be skipped during quick smoke-test cycles.
 */
#define KUNIT_CASE_HEAVY(test_fn) \
    { .name = #test_fn, .run = test_fn, .stress_max = KUNIT_STRESS_HEAVY }

/**
 * KUNIT_CASE_PARAM_DEFAULT() — Declare a parameterized test case with
 *                              default (moderate) stress.
 *
 * @test_fn:  Test function.
 * @params:   NULL-terminated array of &struct kunit_param.
 *
 * See KUNIT_CASE_PARAM() for the same macro without the "_DEFAULT"
 * suffix.
 */
#define KUNIT_CASE_PARAM_DEFAULT(test_fn, params) \
    { .name = #test_fn, .run = test_fn, .params = params, .stress_max = KUNIT_STRESS_MODERATE }

/* ── Fuzz case declaration macros ──────────────────────────────────── */

/**
 * KUNIT_CASE_FUZZ() — Declare a test case with a fuzz parameter
 *                      generator.
 *
 * @test_fn:     Test function.
 * @fuzz_fun:    Fuzz generator callback (kunit_fuzz_fn_t).
 * @fuzz_iters:  Number of fuzz iterations per test.
 *
 * When fuzz mode is enabled, the test runs @fuzz_iters additional
 * iterations with parameters generated by @fuzz_fun.  Defaults to
 * KUNIT_STRESS_HEAVY (skipped during light/moderate runs) because
 * fuzzing is computationally expensive.
 */
#define KUNIT_CASE_FUZZ(test_fn, fuzz_fun, fuzz_iters) \
    { .name = #test_fn, .run = test_fn, .fuzz = fuzz_fun, \
      .fuzz_iters = (fuzz_iters), .stress_max = KUNIT_STRESS_HEAVY }

/**
 * KUNIT_CASE_FUZZ_STRESS() — Like KUNIT_CASE_FUZZ but with an
 *                            explicit stress level.
 *
 * @test_fn:     Test function.
 * @fuzz_fun:    Fuzz generator callback (kunit_fuzz_fn_t).
 * @fuzz_iters:  Number of fuzz iterations per test.
 * @stress:      Stress level for this test case.
 *
 * Useful when a fuzz test is lightweight enough to run during
 * moderate-stress runs.
 */
#define KUNIT_CASE_FUZZ_STRESS(test_fn, fuzz_fun, fuzz_iters, stress) \
    { .name = #test_fn, .run = test_fn, .fuzz = fuzz_fun, \
      .fuzz_iters = (fuzz_iters), .stress_max = (stress) }

/* ── Auto-registration via linker section ────────────────────────── */

/**
 * KUNIT_TEST_SUITE() — Register a suite for automatic registration
 *                       at boot.
 *
 * @suite:  Pointer to a statically-defined &struct kunit_suite.
 *
 * Place this macro invocation after the suite definition in the same
 * source file.  The suite pointer is placed in the .kunit_test_suites
 * linker section and automatically registered during kunit_init().
 * This eliminates the need for manual kunit_register_suite() calls.
 *
 * Usage:
 *   static struct kunit_suite my_suite = { ... };
 *   KUNIT_TEST_SUITE(my_suite);
 */
#define KUNIT_TEST_SUITE(suite) \
    static struct kunit_suite *__kunit_suite_ptr_##suite    \
    __attribute__((__used__, __section__(".kunit_test_suites"))) = &suite

/**
 * __kunit_suites_start — Linker symbol for the start of the
 *                        .kunit_test_suites section.
 *
 * kunit_register_section_suites() iterates between __kunit_suites_start
 * and __kunit_suites_end to register all suites placed in the
 * .kunit_test_suites linker section via KUNIT_TEST_SUITE().
 */
extern struct kunit_suite *__kunit_suites_start[];

/**
 * __kunit_suites_end — Linker symbol for the end of the
 *                      .kunit_test_suites section.
 */
extern struct kunit_suite *__kunit_suites_end[];

#endif /* KUNIT_H */
