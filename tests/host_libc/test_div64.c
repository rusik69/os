/*
 * test_div64.c — Host-side tests for kernel 64-bit division support
 *
 * Tests __udivdi3, __umoddi3, __divdi3, __moddi3, __udivmoddi4, do_div
 * from src/kernel/div64.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern uint64_t __udivdi3(uint64_t a, uint64_t b);
extern uint64_t __umoddi3(uint64_t a, uint64_t b);
extern int64_t __divdi3(int64_t a, int64_t b);
extern int64_t __moddi3(int64_t a, int64_t b);
extern uint64_t __udivmoddi4(uint64_t a, uint64_t b, uint64_t *rem);

/* do_div macro — copied from kernel header */
#define do_div(n, base)                                 \
    ({                                                  \
        uint64_t __rem;                                 \
        uint64_t __base = (uint64_t)(base);             \
        if (__base != 0) {                              \
            __rem = (n) % __base;                       \
            (n) = (n) / __base;                         \
        } else {                                        \
            __rem = 0;                                  \
        }                                               \
        __rem;                                          \
    })

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */
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
 *  test_udivdi3
 * =================================================================== */
static void test_udivdi3(void)
{
    TEST("__udivdi3: 10/2 = 5", __udivdi3(10, 2) == 5);
    TEST("__udivdi3: 0/5 = 0", __udivdi3(0, 5) == 0);
    TEST("__udivdi3: 100/3 = 33", __udivdi3(100, 3) == 33);
    TEST("__udivdi3: 1/1 = 1", __udivdi3(1, 1) == 1);
    TEST("__udivdi3: large/1 = large", __udivdi3(0xFFFFFFFFFFFFFFFFULL, 1) == 0xFFFFFFFFFFFFFFFFULL);
    TEST("__udivdi3: by zero returns 0", __udivdi3(42, 0) == 0);
    TEST("__udivdi3: div exact", __udivdi3(1000000, 1000) == 1000);
    TEST("__udivdi3: large division",
         __udivdi3(0xDEADBEEFCAFEULL, 0x10000) == 0xDEADBEEFCAFEULL / 0x10000);

    /* Edge: 0/0 */
    TEST("__udivdi3: 0/0 = 0", __udivdi3(0, 0) == 0);

    /* Edge: UINT64_MAX/2 */
    TEST("__udivdi3: UINT64_MAX/2",
         __udivdi3(0xFFFFFFFFFFFFFFFFULL, 2) == 0x7FFFFFFFFFFFFFFFULL);

    /* Edge: MAX/MAX */
    TEST("__udivdi3: MAX/MAX = 1",
         __udivdi3(0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL) == 1);
}

/* ===================================================================
 *  test_umoddi3
 * =================================================================== */
static void test_umoddi3(void)
{
    TEST("__umoddi3: 10%3 = 1", __umoddi3(10, 3) == 1);
    TEST("__umoddi3: 100%10 = 0", __umoddi3(100, 10) == 0);
    TEST("__umoddi3: 0%5 = 0", __umoddi3(0, 5) == 0);
    TEST("__umoddi3: 1%1 = 0", __umoddi3(1, 1) == 0);
    TEST("__umoddi3: by zero returns a", __umoddi3(42, 0) == 42);
    TEST("__umoddi3: large mod",
         __umoddi3(0xDEADBEEFCAFEULL, 0x10001) == 0xDEADBEEFCAFEULL % 0x10001);
}

/* ===================================================================
 *  test_divdi3
 * =================================================================== */
static void test_divdi3(void)
{
    TEST("__divdi3: 10/2 = 5", __divdi3(10, 2) == 5);
    TEST("__divdi3: -10/2 = -5", __divdi3(-10, 2) == -5);
    TEST("__divdi3: 10/-2 = -5", __divdi3(10, -2) == -5);
    TEST("__divdi3: -10/-2 = 5", __divdi3(-10, -2) == 5);
    TEST("__divdi3: 0/5 = 0", __divdi3(0, 5) == 0);
    TEST("__divdi3: -1/1 = -1", __divdi3(-1, 1) == -1);
    TEST("__divdi3: by zero returns 0", __divdi3(42, 0) == 0);
    TEST("__divdi3: INT64_MIN / 1", __divdi3(INT64_MIN, 1) == INT64_MIN);
    TEST("__divdi3: INT64_MAX / -1", __divdi3(INT64_MAX, -1) == -INT64_MAX);
    /* INT64_MIN / -1 triggers arithmetic overflow (x86 idiv #DE) — skip */
}

/* ===================================================================
 *  test_moddi3
 * =================================================================== */
static void test_moddi3(void)
{
    TEST("__moddi3: 10%3 = 1", __moddi3(10, 3) == 1);
    TEST("__moddi3: -10%3 = -1", __moddi3(-10, 3) == -1);
    TEST("__moddi3: 10%-3 = 1", __moddi3(10, -3) == 1);
    TEST("__moddi3: -10%-3 = -1", __moddi3(-10, -3) == -1);
    TEST("__moddi3: 0%5 = 0", __moddi3(0, 5) == 0);
    TEST("__moddi3: by zero returns a", __moddi3(42, 0) == 42);
    TEST("__moddi3: neg_mod", __moddi3(-100, 7) == -100 % 7);
    /* INT64_MIN % -1 involves INT64_MIN / -1 which overflows — skip */
    TEST("__moddi3: INT64_MIN % 1 = 0", __moddi3(INT64_MIN, 1) == 0);
    TEST("__moddi3: 0 %% -1 = 0", __moddi3(0, -1) == 0);
}

/* ===================================================================
 *  test_udivmoddi4
 * =================================================================== */
static void test_udivmoddi4(void)
{
    uint64_t rem;

    /* 1. Basic */
    uint64_t q = __udivmoddi4(100, 7, &rem);
    TEST("__udivmoddi4: quotient", q == 14);
    TEST("__udivmoddi4: remainder", rem == 2);

    /* 2. Exact */
    q = __udivmoddi4(81, 9, &rem);
    TEST("__udivmoddi4: exact quotient", q == 9);
    TEST("__udivmoddi4: exact remainder 0", rem == 0);

    /* 3. Large */
    q = __udivmoddi4(0xDEADBEEFCAFEULL, 0x1000, &rem);
    TEST("__udivmoddi4: large quotient",
         q == 0xDEADBEEFCAFEULL / 0x1000);
    TEST("__udivmoddi4: large remainder",
         rem == 0xDEADBEEFCAFEULL % 0x1000);

    /* 4. By zero */
    q = __udivmoddi4(42, 0, &rem);
    TEST("__udivmoddi4: by zero quotient 0", q == 0);
    TEST("__udivmoddi4: by zero remainder = a", rem == 42);

    /* 5. 0/0 */
    q = __udivmoddi4(0, 0, &rem);
    TEST("__udivmoddi4: 0/0 quotient 0", q == 0);
    TEST("__udivmoddi4: 0/0 remainder 0", rem == 0);

    /* 6. UINT64_MAX/1 */
    q = __udivmoddi4(0xFFFFFFFFFFFFFFFFULL, 1, &rem);
    TEST("__udivmoddi4: MAX/1 quotient MAX", q == 0xFFFFFFFFFFFFFFFFULL);
    TEST("__udivmoddi4: MAX/1 remainder 0", rem == 0);
}

/* ===================================================================
 *  test_do_div
 * =================================================================== */
static void test_do_div(void)
{
    uint64_t n;

    /* 1. Basic */
    n = 1000;
    uint32_t rem = do_div(n, 7);
    TEST("do_div: quotient 142", n == 142);
    TEST("do_div: remainder 6", rem == 6);

    /* 2. Exact */
    n = 81;
    rem = do_div(n, 9);
    TEST("do_div: exact quotient 9", n == 9);
    TEST("do_div: exact remainder 0", rem == 0);

    /* 3. By 1 */
    n = 999;
    rem = do_div(n, 1);
    TEST("do_div: /1 quotient", n == 999);
    TEST("do_div: /1 remainder", rem == 0);

    /* 4. Large */
    n = 1000000;
    rem = do_div(n, 1000);
    TEST("do_div: large quotient", n == 1000);
    TEST("do_div: large remainder", rem == 0);

    /* 5. Zero division */
    n = 42;
    rem = do_div(n, 0);
    TEST("do_div: by zero quotient unchanged", n == 42);
    TEST("do_div: by zero remainder 0", rem == 0);

    /* 6. n=0 */
    n = 0;
    rem = do_div(n, 10);
    TEST("do_div: n=0 quotient", n == 0);
    TEST("do_div: n=0 remainder", rem == 0);

    /* 7. UINT64_MAX / 2 */
    n = 0xFFFFFFFFFFFFFFFFULL;
    rem = do_div(n, 2);
    TEST("do_div: UINT64_MAX/2 quotient", n == 0x7FFFFFFFFFFFFFFFULL);
    TEST("do_div: UINT64_MAX/2 remainder", rem == 1);

    /* 8. 1 / UINT64_MAX = 0, remainder = 1 */
    n = 1;
    rem = do_div(n, 0xFFFFFFFFFFFFFFFFULL);
    TEST("do_div: 1/MAX quotient 0", n == 0);
    TEST("do_div: 1/MAX remainder 1", rem == 1);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== 64-bit Division Tests ===\n\n");

    printf("--- __udivdi3 ---\n");
    test_udivdi3();

    printf("\n--- __umoddi3 ---\n");
    test_umoddi3();

    printf("\n--- __divdi3 ---\n");
    test_divdi3();

    printf("\n--- __moddi3 ---\n");
    test_moddi3();

    printf("\n--- __udivmoddi4 ---\n");
    test_udivmoddi4();

    printf("\n--- do_div macro ---\n");
    test_do_div();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
