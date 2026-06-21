/*
 * kunit_container_ext.c — KUnit test suites for enhanced container exec.
 *
 * Tests container_exec_enhanced functions:
 *   container_exec_attach, container_exec_resize,
 *   container_exec_write_stdin, container_exec_read_stdout,
 *   container_exec_detach
 */

#include "kunit.h"
#include "container_exec_enhanced.h"
#include "container.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ═══════════════════════════════════════════════════════════════════════
 *  1. Container Exec Enhanced Tests
 * ═══════════════════════════════════════════════════════════════════════ */

static void exec_attach_null_check_test(struct kunit *test)
{
    /* Test with NULL container_id — should return NULL */
    struct exec_attach *ea = container_exec_attach(
        NULL, "/bin/echo", NULL, NULL, 0);
    KUNIT_EXPECT_NULL(test, ea);
}

static void exec_attach_null_binary_test(struct kunit *test)
{
    /* Test with NULL binary — should return NULL */
    struct exec_attach *ea = container_exec_attach(
        "test-container", NULL, NULL, NULL, 0);
    KUNIT_EXPECT_NULL(test, ea);
}

static void exec_detach_null_test(struct kunit *test)
{
    /* Detach with NULL should return -EINVAL */
    int ret = container_exec_detach(NULL);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void exec_resize_null_test(struct kunit *test)
{
    /* Resize with NULL should return -EINVAL */
    int ret = container_exec_resize(NULL, 80, 24);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void exec_write_stdin_null_test(struct kunit *test)
{
    /* Write stdin with NULL should return -EINVAL */
    int ret = container_exec_write_stdin(NULL, "hello", 5);
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void exec_read_stdout_null_test(struct kunit *test)
{
    /* Read stdout with NULL should return -EINVAL */
    char buf[64];
    int ret = container_exec_read_stdout(NULL, buf, sizeof(buf));
    KUNIT_EXPECT_NE(test, ret, 0);
}

static void exec_read_stderr_null_test(struct kunit *test)
{
    /* Read stderr with NULL should return -EINVAL */
    char buf[64];
    int ret = container_exec_read_stderr(NULL, buf, sizeof(buf));
    KUNIT_EXPECT_NE(test, ret, 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Test case lists
 * ═══════════════════════════════════════════════════════════════════════ */

static struct kunit_case exec_enhanced_test_cases[] = {
    KUNIT_CASE(exec_attach_null_check_test),
    KUNIT_CASE(exec_attach_null_binary_test),
    KUNIT_CASE(exec_detach_null_test),
    KUNIT_CASE(exec_resize_null_test),
    KUNIT_CASE(exec_write_stdin_null_test),
    KUNIT_CASE(exec_read_stdout_null_test),
    KUNIT_CASE(exec_read_stderr_null_test),
    {0}
};

/* ── Suite declaration ─────────────────────────────────────────────── */

static struct kunit_suite exec_enhanced_test_suite;

/* ═══════════════════════════════════════════════════════════════════════
 *  Suite Registration
 * ═══════════════════════════════════════════════════════════════════════ */

void kunit_container_ext_register(void)
{
    /* ── Exec Enhanced ── */
    {
        int ci = 0;
        for (int i = 0; exec_enhanced_test_cases[i].run != NULL &&
             i < KUNIT_MAX_CASES - 1; i++) {
            exec_enhanced_test_suite.cases[ci].name =
                exec_enhanced_test_cases[i].name;
            exec_enhanced_test_suite.cases[ci].run  =
                exec_enhanced_test_cases[i].run;
            ci++;
        }
        exec_enhanced_test_suite.cases[ci].name = NULL;
        exec_enhanced_test_suite.cases[ci].run  = NULL;
        exec_enhanced_test_suite.name  = "container_exec_enhanced";
        exec_enhanced_test_suite.setup = NULL;
        exec_enhanced_test_suite.teardown = NULL;
        kunit_register_suite(&exec_enhanced_test_suite);
    }

    kprintf("[KUnit] Container exec enhanced tests registered\n");
}

/* ── Stub: kunit_container_ext_init ─────────────────────────────── */
int kunit_container_ext_init(void)
{
    kprintf("[kunit] kunit_container_ext_init: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_container_ext_test_create ─────────────────────────────── */
int kunit_container_ext_test_create(void)
{
    kprintf("[kunit] kunit_container_ext_test_create: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: kunit_container_ext_test_start ─────────────────────────────── */
int kunit_container_ext_test_start(void)
{
    kprintf("[kunit] kunit_container_ext_test_start: not yet implemented\n");
    return -ENOSYS;
}
