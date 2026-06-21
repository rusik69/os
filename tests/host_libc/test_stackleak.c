/*
 * test_stackleak.c — Host-side tests for kernel stack eraser (STACKLEAK)
 *
 * Tests stackleak_get_enabled, stackleak_set_enabled,
 * stackleak_get_poison_count from src/kernel/stackleak.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ===================================================================
 *  Kernel type declarations
 * =================================================================== */

/* The process struct — stackleak.c accesses p->kernel_stack */
struct process {
    uint64_t kernel_stack;
    int pid;
};

/* ── Extern declarations ─────────────────────────────────────────── */

extern int stackleak_get_enabled(void);
extern int stackleak_set_enabled(int val);
extern uint64_t stackleak_get_poison_count(void);
extern void stackleak_init(void);

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* process_get_current stub — provided by stubs.c */
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
 *  test_stackleak
 * =================================================================== */

static void test_stackleak(void)
{
    /* 0. Init doesn't crash */
    stackleak_init();

    /* 1. Default state: enabled */
    {
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: default is 1", enabled == 1);
    }

    /* 2. Initial poison count is 0 */
    {
        uint64_t count = stackleak_get_poison_count();
        TEST("get_poison_count: initial is 0", count == 0ULL);
    }

    /* 3. Disable: set_enabled(0) returns old value (1) */
    {
        int old = stackleak_set_enabled(0);
        TEST("set_enabled(0): returns old value 1", old == 1);
    }

    /* 4. After disable, get_enabled returns 0 */
    {
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: now 0 after disable", enabled == 0);
    }

    /* 5. Re-enable: set_enabled(1) returns old value (0) */
    {
        int old = stackleak_set_enabled(1);
        TEST("set_enabled(1): returns old value 0", old == 0);
    }

    /* 6. After re-enable, get_enabled returns 1 */
    {
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 1 after re-enable", enabled == 1);
    }

    /* 7. Double disable: set_enabled(0) → 1; then again → 0 */
    {
        int old1 = stackleak_set_enabled(0);
        int old2 = stackleak_set_enabled(0);
        TEST("set_enabled(0) twice: first returns 1", old1 == 1);
        TEST("set_enabled(0) twice: second returns 0", old2 == 0);
    }

    /* 8. set_enabled with non-zero value coerces to 1 */
    {
        stackleak_set_enabled(1);
        int old = stackleak_set_enabled(42);
        TEST("set_enabled(42): non-zero treated as 1, returns old 1", old == 1);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 1 after set_enabled(42) (42 coerced to 1)", enabled == 1);
    }

    /* 9. set_enabled with zero disables */
    {
        stackleak_set_enabled(1);
        int old = stackleak_set_enabled(0);
        TEST("set_enabled(0): returns 1 after re-enable", old == 1);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 0 after disable", enabled == 0);
        (void)enabled;
    }

    /* 10. Multiple toggles */
    {
        stackleak_set_enabled(0);
        stackleak_set_enabled(1);
        stackleak_set_enabled(0);
        stackleak_set_enabled(1);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 1 after multiple toggles", enabled == 1);
    }

    /* 11. Poison count not affected by get/set operations */
    {
        uint64_t count_before = stackleak_get_poison_count();
        stackleak_set_enabled(0);
        stackleak_set_enabled(1);
        uint64_t count_after = stackleak_get_poison_count();
        TEST("get_poison_count: unchanged by get/set ops",
             count_after == count_before);
    }

    /* 12. Poison count is readable and returns sensible value */
    {
        uint64_t count = stackleak_get_poison_count();
        TEST("get_poison_count: returns value <= 1ULL<<63",
             count < (1ULL << 63));
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== STACKLEAK Kernel Stack Eraser Tests ===\n\n");
    test_stackleak();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
