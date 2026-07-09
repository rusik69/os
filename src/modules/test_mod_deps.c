/* test_mod_deps.c — Dependency test module for module-to-module symbol
 * resolution testing (D233 task 4).
 *
 * This module uses symbols exported by test_module.ko to verify that
 * the module loader correctly resolves cross-module symbol references.
 *
 * It depends on test_module being loaded first (MODULE_DEPENDS).
 *
 * Usage:
 *   insmod /modules/test_module.ko
 *   insmod /modules/test_mod_deps.ko
 *   rmmod test_mod_deps
 *   rmmod test_module
 */

#include "module.h"
#include "printf.h"
#include "export.h"

/* ── Symbols imported from test_module ────────────────────────────
 *
 * These functions are exported by test_module.ko via EXPORT_SYMBOL
 * and EXPORT_SYMBOL_GPL.  The module loader must resolve them from
 * the dynamic symbol table (ksym_register) when loading this module.
 */

extern int test_mod_add(int a, int b);
extern int test_mod_mul(int a, int b);

/* ── State tracking ─────────────────────────────────────────────── */

enum test_dep_state {
    TEST_DEP_UNINITIALIZED = 0,
    TEST_DEP_LOADED,
    TEST_DEP_EXITED,
};

static enum test_dep_state g_test_dep_state = TEST_DEP_UNINITIALIZED;
static int g_test_dep_init_count = 0;
static int g_test_dep_exit_count = 0;

/* Verify that cross-module symbol resolution works.
 * Returns 0 on success, -1 on failure. */
static int test_dep_verify_symbols(void)
{
    int result;

    /* Test test_mod_add (public export) */
    result = test_mod_add(10, 20);
    if (result != 30) {
        kprintf("[TEST_DEP] FAIL: test_mod_add(10, 20) = %d, expected 30\n",
                result);
        return -1;
    }

    /* Test test_mod_mul (GPL-only export) */
    result = test_mod_mul(5, 7);
    if (result != 35) {
        kprintf("[TEST_DEP] FAIL: test_mod_mul(5, 7) = %d, expected 35\n",
                result);
        return -1;
    }

    kprintf("[TEST_DEP] Symbol resolution OK: add=30, mul=35\n");
    return 0;
}

/* ── Module entry points ────────────────────────────────────────── */

int __init init_module(void)
{
    kprintf("[TEST_DEP] init_module called\n");

    g_test_dep_init_count++;
    g_test_dep_state = TEST_DEP_LOADED;

    if (test_dep_verify_symbols() < 0) {
        kprintf("[TEST_DEP] Symbol verification FAILED — unloading\n");
        g_test_dep_state = TEST_DEP_EXITED;
        return -1;
    }

    kprintf("[TEST_DEP] init_module complete (count=%d)\n",
            g_test_dep_init_count);
    return 0;
}

void __exit cleanup_module(void)
{
    kprintf("[TEST_DEP] cleanup_module called\n");
    g_test_dep_exit_count++;
    g_test_dep_state = TEST_DEP_EXITED;
    kprintf("[TEST_DEP] cleanup_module complete (count=%d)\n",
            g_test_dep_exit_count);
}

/* ── Module metadata ──────────────────────────────────────────── */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("OS Kernel Team");
MODULE_DESCRIPTION("Test module for cross-module symbol resolution (D233)");
MODULE_VERSION("1.0");
MODULE_DEPENDS("test_module");
