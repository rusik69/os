#ifndef TEST_H
#define TEST_H

/* Called by kernel_main when compiled with -DTEST_MODE.
 * Runs all subsystem tests, prints PASS/FAIL lines to serial, then
 * prints "ALL TESTS PASSED" or "SOME TESTS FAILED" and calls acpi_shutdown(). */
void test_run_all(void);

#endif
