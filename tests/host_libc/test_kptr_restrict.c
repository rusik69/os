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

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

struct process { int pid; int euid; int uid; unsigned char sched_policy; uint8_t priority; };

/* Our stub: process_get_current returns NULL (kernel thread context) */
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
 *  Test: kptr_restrict
 * =================================================================== */

static void test_kptr_restrict(void)
{
    printf("\n[kptr_restrict]\n");

    /* 1. Initial value should be KPTR_RESTRICT_RESTRICTED (1) */
    TEST("kptr_restrict: initial value == RESTRICTED",
         kptr_restrict == KPTR_RESTRICT_RESTRICTED);

    /* 2. kptr_restrict_check() with default value.
     *    Since process_get_current() returns NULL (kernel thread),
     *    kptr_restrict_check() returns 0 for kernel threads at level 1. */
    {
        int r = kptr_restrict_check();
        /* With our stub (process_get_current returns NULL), the code
         * path says: if (!p) return 0; so returns 0 at level 1. */
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
        TEST("kptr_restrict_check: ROOT_HIDE returns 1",
             r == 1);
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
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel Pointer Restrict Tests ===\n");
    test_kptr_restrict();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
