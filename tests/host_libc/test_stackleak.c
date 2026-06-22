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

    /* 13. set_enabled with negative value (-1) coerces to 1 */
    {
        stackleak_set_enabled(0);
        int old = stackleak_set_enabled(-1);
        TEST("set_enabled(-1): negative coerces to 1, returns old 0", old == 0);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 1 after set_enabled(-1)", enabled == 1);
    }

    /* 14. set_enabled with large positive value coerces to 1 */
    {
        stackleak_set_enabled(0);
        int old = stackleak_set_enabled(0x7FFFFFFF);
        TEST("set_enabled(0x7FFFFFFF): large value coerces to 1", old == 0);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: 1 after set_enabled(0x7FFFFFFF)", enabled == 1);
    }

    /* 15. Disabled state persists across multiple gets */
    {
        stackleak_set_enabled(0);
        int g1 = stackleak_get_enabled();
        int g2 = stackleak_get_enabled();
        int g3 = stackleak_get_enabled();
        TEST("get_enabled: persistent 0 after disable (1st call)", g1 == 0);
        TEST("get_enabled: persistent 0 after disable (2nd call)", g2 == 0);
        TEST("get_enabled: persistent 0 after disable (3rd call)", g3 == 0);
    }

    /* 16. set_enabled(1) when already enabled—idempotent */
    {
        stackleak_set_enabled(1);
        int old = stackleak_set_enabled(1);
        TEST("set_enabled(1) when already 1 returns 1", old == 1);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: still 1 after double-enable", enabled == 1);
    }

    /* 17. set_enabled(0) when already disabled—idempotent */
    {
        stackleak_set_enabled(0);
        int old = stackleak_set_enabled(0);
        TEST("set_enabled(0) when already 0 returns 0", old == 0);
        int enabled = stackleak_get_enabled();
        TEST("get_enabled: still 0 after double-disable", enabled == 0);
    }

    /* 18. Rapid toggles: 5 pairs of flip-flop */
    {
        stackleak_set_enabled(0);
        for (int i = 0; i < 5; i++) {
            stackleak_set_enabled(1);
            stackleak_set_enabled(0);
        }
        TEST("get_enabled: 0 after 5 rapid toggle pairs",
             stackleak_get_enabled() == 0);
    }

    /* 19. set_enabled returns correct chain of old values */
    {
        stackleak_set_enabled(1);
        stackleak_set_enabled(0);
        int v1 = stackleak_set_enabled(1);  /* 0→1 returns 0 */
        int v2 = stackleak_set_enabled(0);  /* 1→0 returns 1 */
        int v3 = stackleak_set_enabled(1);  /* 0→1 returns 0 */
        TEST("set_enabled chain: 0→1 returns 0", v1 == 0);
        TEST("set_enabled chain: 1→0 returns 1", v2 == 1);
        TEST("set_enabled chain: 0→1 returns 0 again", v3 == 0);
    }

    /* 20. set_enabled with five different large values */
    {
        stackleak_set_enabled(0);
        int e = stackleak_set_enabled(42);
        TEST("set_enabled(42): returns old 0", e == 0);
        TEST("get_enabled: 1 after 42", stackleak_get_enabled() == 1);
        e = stackleak_set_enabled(0);
        TEST("set_enabled(0): returns old 1", e == 1);
        e = stackleak_set_enabled(-100);
        TEST("set_enabled(-100): returns old 0", e == 0);
        TEST("get_enabled: finally 1", stackleak_get_enabled() == 1);
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
