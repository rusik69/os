#ifndef TEST_H
#define TEST_H

/**
 * DOC: Kernel Test Framework
 *
 * The OS kernel provides a dual test framework:
 *
 * 1. LEGACY ASSERT-BASED TESTS  (test.c)
 *    - Compiled into the kernel when -DTEST_MODE is set.
 *    - Uses simple macros ASSERT(), ASSERT_EQ(), ASSERT_STR().
 *    - All tests run sequentially from test_run_all().
 *    - Output: "[PASS]" / "[FAIL]" lines to serial console.
 *    - Verdict: "ALL TESTS PASSED" or "SOME TESTS FAILED".
 *    - Best for: quick smoke tests, simple verification.
 *
 * 2. KUNIT FRAMEWORK  (kunit.h / kunit.c)
 *    - Lightweight in-kernel unit test framework (inspired by Linux KUnit).
 *    - Always compiled in (no -DTEST_MODE required).
 *    - Exposes debugfs interface at /sys/kernel/debug/kunit/.
 *    - Uses modern macros: KUNIT_EXPECT_EQ(), KUNIT_EXPECT_TRUE(), etc.
 *    - Supports test suite grouping (setup/teardown).
 *    - Supports parameterized, fuzz, and stress-level-controlled tests.
 *    - Supports parallel execution, coverage tracking, regression
 *      detection, result export (JSON), and log buffer management.
 *    - Best for: structured, reusable unit tests with rich features.
 *
 * FRAMEWORK CHOICE:
 *   - For new tests that need isolation, parameterization, or fuzzing:
 *     use the KUnit framework.  See &lt;kunit.h&gt; for the full API.
 *   - For quick regression tests or simple in-kernel verification
 *     during boot: use ASSERT() macros from test.c.
 *   - Both can coexist — test.c exercises subsystems that also have
 *     KUnit test suites.
 *
 * TEST LIFECYCLE:
 *   - Legacy:  ASSERT(name, cond) → [PASS] / [FAIL] on serial
 *   - KUnit:   KUNIT_EXPECT_EQ(test, actual, expected) →
 *              PASS/FAIL tracked per test case, aggregated per suite
 *
 * ADDING A NEW KUNIT TEST:
 *   1. Create a test .c file in src/test/ (e.g. kunit_foo.c).
 *   2. Include "kunit.h".
 *   3. Define test function(s) using KUNIT_EXPECT_* macros.
 *   4. Create a struct kunit_case array.
 *   5. Create a struct kunit_suite.
 *   6. Add the suite registration to kunit_register_builtin_tests()
 *      in src/test/kunit_tests.c.
 *   7. Add the .c file to C_SRCS in the Makefile.
 *
 * ADDING A LEGACY TEST:
 *   1. Add a static test_* function in src/test/test.c.
 *   2. Use ASSERT(), ASSERT_EQ(), or ASSERT_STR() macros.
 *   3. Call the function from test_run_all().
 */

/* Called by kernel_main when compiled with -DTEST_MODE.
 * Runs all subsystem tests, prints PASS/FAIL lines to serial, then
 * prints "ALL TESTS PASSED" or "SOME TESTS FAILED" and calls
 * acpi_shutdown().  Test functions are defined in src/test/test.c
 * and use the ASSERT / ASSERT_EQ / ASSERT_STR macros.
 *
 * Note: This only executes the legacy (pre-KUnit) test suite.
 * KUnit tests are registered via kunit_init() and run separately
 * through the debugfs interface or via kernel_main when configured. */
void test_run_all(void);

#endif /* TEST_H */
