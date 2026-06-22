/*
 * test_kptr_restrict.c — Host-side tests for kernel pointer restrict.
 *
 * Tests kptr_restrict_check, kptr_restrict_init from src/kernel/kptr_restrict.c.
 *
 * Compile: part of host_libc test suite (via Makefile)
 * NOTE: Does NOT link stubs.o because both define kptr_restrict_check().
 * Instead, required stubs are defined inline here.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel constant declarations
 * =================================================================== */

#define KPTR_RESTRICT_DISABLED      0
#define KPTR_RESTRICT_RESTRICTED    1
#define KPTR_RESTRICT_ROOT_HIDE     2

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

extern int kptr_restrict;
extern void kptr_restrict_init(void);
extern int  kptr_restrict_check(void);
extern int  kptr_restrict_set(int level);
extern int  kptr_restrict_get(void);

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

/* CAUTION: The kernel's struct process (in process.h) is large and
 * has a completely different layout from this stub.  process_get_current
 * returns NULL here, which is the safe path in kptr_restrict_check().
 * Testing with a non-NULL process is not feasible without either
 * including the full kernel process.h (which pulls in many dependencies)
 * or duplicating its exact layout — both are fragile.  We stick to
 * the NULL-process path for all kptr_restrict_check tests, which still
 * exercises the level-detection logic (levels 0, 1, 2, negative, large). */

struct process { int pid; int euid; int uid; unsigned char sched_policy; uint8_t priority; };

struct process *process_get_current(void) { return NULL; }

/* Sysctl stub */
void sysctl_register(const char *name,
                     int (*read_fn)(char *, int),
                     int (*write_fn)(const char *, int)) {
    (void)name; (void)read_fn; (void)write_fn;
}

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* ===================================================================
 *  Test harness
 * =================================================================== */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name, cond) do {                                           \
    if (!(cond)) {                                                      \
        printf("  FAIL: %s (%s)\n", name, #cond);                      \
        tests_failed++;                                                 \
    } else {                                                            \
        printf("  PASS: %s\n", name);                                   \
        tests_passed++;                                                 \
    }                                                                   \
} while (0)

/* ===================================================================
 *  Test: kptr_restrict — baseline tests (6 existing)
 * =================================================================== */

static void test_kptr_restrict(void)
{
    printf("\n[kptr_restrict — baseline]\n");

    /* 1. Initial value should be KPTR_RESTRICT_RESTRICTED (1) */
    TEST("kptr_restrict: initial value == RESTRICTED",
         kptr_restrict == KPTR_RESTRICT_RESTRICTED);

    /* 2. kptr_restrict_check() with default value.
     *    Since process_get_current() returns NULL (kernel thread),
     *    kptr_restrict_check() returns 0 for kernel threads at level 1. */
    {
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: at level 1 with NULL process returns 0",
             r == 0);
    }

    /* 3. Set to KPTR_RESTRICT_DISABLED (0) → check returns 0 */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: DISABLED returns 0", r == 0);
    }

    /* 4. Set to KPTR_RESTRICT_ROOT_HIDE (2) → check returns 1 (hide all) */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: ROOT_HIDE returns 1", r == 1);
    }

    /* 5. Restore to RESTRICTED (1) → still returns 0 for NULL process */
    {
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: RESTRICTED with NULL process returns 0",
             r == 0);
    }

    /* 6. kptr_restrict_init doesn't crash */
    {
        kptr_restrict_init();
        TEST("kptr_restrict_init: no crash", 1);
    }
}

/* ===================================================================
 *  Test: kptr_restrict — extended tests (+15 new assertions)
 * =================================================================== */

static void test_kptr_restrict_extended(void)
{
    printf("\n[kptr_restrict — extended]\n");

    /* --- Tests with NULL (kernel-thread) process --- */

    /* 1. Level 0: explicit return 0 */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: level 0, NULL proc, returns 0", r == 0);
    }

    /* 2. Level 1: NULL process → returns 0 (kernel thread) */
    {
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: level 1, NULL proc, returns 0", r == 0);
    }

    /* 3. Level 2: hide from everyone */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: level 2, NULL proc, returns 1", r == 1);
    }

    /* 4. Negative level (-1): falls through to level-1 logic, NULL → 0 */
    {
        kptr_restrict = -1;
        TEST("kptr_restrict_check: level -1, NULL proc, returns 0",
             kptr_restrict_check() == 0);
    }

    /* 5. Out-of-range high level (999): >= ROOT_HIDE → returns 1 */
    {
        kptr_restrict = 999;
        TEST("kptr_restrict_check: level 999, NULL proc, returns 1",
             kptr_restrict_check() == 1);
    }

    /* 6. kptr_restrict_set returns 0 (stub) */
    {
        int ret = kptr_restrict_set(1);
        TEST("kptr_restrict_set returns 0", ret == 0);
    }

    /* 7. kptr_restrict_get returns 0 (stub) */
    {
        int ret = kptr_restrict_get();
        TEST("kptr_restrict_get returns 0", ret == 0);
    }

    /* 8. Multiple calls at same level return consistent results */
    {
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        int r1 = kptr_restrict_check();
        int r2 = kptr_restrict_check();
        TEST("kptr_restrict_check: consistent results at level 1",
             r1 == 0 && r2 == 0);
    }

    /* 9. Level transition: 0→2→0 returns correct values */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int r0 = kptr_restrict_check();
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        int r2 = kptr_restrict_check();
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int r0b = kptr_restrict_check();
        TEST("kptr_restrict_check: 0→2→0 transition correct",
             r0 == 0 && r2 == 1 && r0b == 0);
    }

    /* 10. kptr_restrict_init preserves current kptr_restrict value */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        kptr_restrict_init();
        TEST("kptr_restrict_init preserves level 2 value",
             kptr_restrict == KPTR_RESTRICT_ROOT_HIDE);
        int r = kptr_restrict_check();
        TEST("kptr_restrict: after init, level 2 still returns 1", r == 1);
    }

    /* 11. kptr_restrict_set does NOT change the global (it's a stub) */
    {
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        kptr_restrict_set(0);
        TEST("kptr_restrict_set: global unchanged after set stub",
             kptr_restrict == KPTR_RESTRICT_RESTRICTED);
    }

    /* 12. Very large positive level (INT_MAX) hides all */
    {
        kptr_restrict = 2147483647;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: level INT_MAX (>=2) returns 1", r == 1);
    }

    /* 13. Level -2: still falls through to level-1 logic, NULL → 0 */
    {
        kptr_restrict = -2;
        int r = kptr_restrict_check();
        TEST("kptr_restrict_check: level -2 returns 0", r == 0);
    }

    /* 14. Level 1 but with explicit check sequence (DISABLED→RESTRICTED) */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        TEST("level 0 pre-check returns 0", kptr_restrict_check() == 0);
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        TEST("level 1 post-transition returns 0 (NULL proc)",
             kptr_restrict_check() == 0);
    }

    /* 15. Check that kptr_restrict_init can be called after value change */
    {
        kptr_restrict = 42;  /* garbage value, not valid level */
        kptr_restrict_init();
        TEST("kptr_restrict_init with arbitrary value: no crash", 1);
        /* Restore to a known level */
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
    }
}

/* ===================================================================
 *  Test: kptr_restrict — format pointer tests (+15 new assertions)
 * =================================================================== */

/* Simulate %pK printing behaviour: calls kptr_restrict_check() to
 * decide whether to show the real pointer or substitute zeros. */
static int should_show_pointer(void)
{
    return kptr_restrict_check() == 0;
}

static void test_kptr_restrict_format(void)
{
    printf("\n[kptr_restrict — format %%pK vs %%p]\n");

    /* 1. At level 0 (disabled), pointers should be visible */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        TEST("format: level 0 shows pointer", should_show_pointer() == 1);
    }

    /* 2. At level 1 (restricted), NULL process → kernel thread → show */
    {
        kptr_restrict = KPTR_RESTRICT_RESTRICTED;
        TEST("format: level 1 (kernel thread) shows pointer",
             should_show_pointer() == 1);
    }

    /* 3. At level 2 (root hide), pointer hidden */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        TEST("format: level 2 hides pointer", should_show_pointer() == 0);
    }

    /* 4. Negative level -1 shows (follows level-1 path) */
    {
        kptr_restrict = -1;
        TEST("format: level -1 shows pointer", should_show_pointer() == 1);
    }

    /* 5. Large positive level >=2 hides */
    {
        kptr_restrict = 100;
        TEST("format: level 100 hides pointer", should_show_pointer() == 0);
    }

    /* 6. Transition path: level 2 → 0 → show */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        int hide_state = should_show_pointer();
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int show_state = should_show_pointer();
        TEST("format: level 2→0 transition correct",
             hide_state == 0 && show_state == 1);
    }

    /* 7. kptr_restrict_check level 0: should_show = 1 (check returns 0) */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int r = kptr_restrict_check();
        int show = should_show_pointer();
        TEST("format: level 0 check returns 0", r == 0);
        TEST("format: level 0 should_show is 1", show == 1);
    }

    /* 8. kptr_restrict_check() called 10 times at level 0 stays 0 */
    {
        kptr_restrict = KPTR_RESTRICT_DISABLED;
        int all_zero = 1;
        for (int i = 0; i < 10; i++) {
            if (kptr_restrict_check() != 0) { all_zero = 0; break; }
        }
        TEST("kptr_restrict: 10 calls at level 0 all return 0", all_zero);
    }

    /* 9. kptr_restrict_check() called 10 times at level 2 stays 1 */
    {
        kptr_restrict = KPTR_RESTRICT_ROOT_HIDE;
        int all_one = 1;
        for (int i = 0; i < 10; i++) {
            if (kptr_restrict_check() != 1) { all_one = 0; break; }
        }
        TEST("kptr_restrict: 10 calls at level 2 all return 1", all_one);
    }

    /* 10. Check at level 0, 1, 2 in rapid succession.
     *     Level 0 (DISABLED): check() returns 0 → should_show = 1
     *     Level 1 (RESTRICTED, NULL proc): check() returns 0 → should_show = 1
     *     Level 2 (ROOT_HIDE): check() returns 1 → should_show = 0 */
    {
        int show[3];
        kptr_restrict = 0; show[0] = should_show_pointer();
        kptr_restrict = 1; show[1] = should_show_pointer();
        kptr_restrict = 2; show[2] = should_show_pointer();
        TEST("format: level 0→1→2 should_show sequence",
             show[0] == 1 && show[1] == 1 && show[2] == 0);
    }

    /* 11. kptr_restrict_set/kptr_restrict_get both return 0 (stubs) */
    {
        TEST("kptr_restrict_set(0) returns 0", kptr_restrict_set(0) == 0);
        TEST("kptr_restrict_set(-1) returns 0", kptr_restrict_set(-1) == 0);
        TEST("kptr_restrict_set(2) returns 0", kptr_restrict_set(2) == 0);
        TEST("kptr_restrict_get() returns 0", kptr_restrict_get() == 0);
    }

    /* 12. Very large negative level (-1000000) — same as -1 path */
    {
        kptr_restrict = -1000000;
        TEST("format: level -1000000 shows pointer (same as -1 path)",
             should_show_pointer() == 1);
    }

    /* Restore default */
    kptr_restrict = KPTR_RESTRICT_RESTRICTED;
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Pointer Restrict Tests ===\n");
    test_kptr_restrict();
    test_kptr_restrict_extended();
    test_kptr_restrict_format();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
