/*
 * test_find_bit.c — Host-side tests for kernel find_bit operations
 *
 * Tests test_bit, set_bit, clear_bit, find_first_bit, find_first_zero_bit,
 * find_last_bit, find_next_bit, find_next_zero_bit from src/lib/find_bit.c.
 * Pure algorithmic — uses __sync builtins available on host.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern int test_bit(int nr, const volatile unsigned long *addr);
extern void set_bit(int nr, volatile unsigned long *addr);
extern void clear_bit(int nr, volatile unsigned long *addr);
extern unsigned long find_first_bit(const unsigned long *addr, unsigned long size);
extern unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size);
extern unsigned long find_last_bit(const unsigned long *addr, unsigned long size);
extern unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset);
extern unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset);

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)     { (void)c; }
void serial_putchar(char c)  { (void)c; }

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
 *  test_set_clear_bit
 * =================================================================== */
static void test_set_clear_bit(void)
{
    unsigned long word = 0;

    /* 1. test_bit on clear bit */
    TEST("test_bit: clear bit 0 returns 0", test_bit(0, &word) == 0);

    /* 2. set bit 0 */
    set_bit(0, &word);
    TEST("set_bit: bit 0 set", test_bit(0, &word) != 0);

    /* 3. set bit 5 */
    set_bit(5, &word);
    TEST("set_bit: bit 5 set", test_bit(5, &word) != 0);
    TEST("set_bit: bit 0 still set", test_bit(0, &word) != 0);

    /* 4. clear bit 0 */
    clear_bit(0, &word);
    TEST("clear_bit: bit 0 cleared", test_bit(0, &word) == 0);
    TEST("clear_bit: bit 5 still set", test_bit(5, &word) != 0);

    /* 5. Bit at word boundary — bit 63 on 64-bit */
    unsigned long w2 = 0;
    set_bit(63, &w2);
    TEST("set_bit: bit 63 set", test_bit(63, &w2) != 0);
    clear_bit(63, &w2);
    TEST("clear_bit: bit 63 cleared", test_bit(63, &w2) == 0);

    /* 6. Multi-word: bit 64+ uses array indexing */
    unsigned long arr[3] = {0, 0, 0};
    set_bit(64, arr);
    TEST("set_bit: bit 64 in second word", test_bit(64, arr) != 0);
    TEST("set_bit: bit 64 not in first word", test_bit(0, arr) == 0);
    set_bit(128, arr);
    TEST("set_bit: bit 128 in third word", test_bit(128, arr) != 0);
}

/* ===================================================================
 *  test_find_first_bit
 * =================================================================== */
static void test_find_first_bit(void)
{
    unsigned long arr[2] = {0, 0};

    /* 1. Empty — returns size */
    TEST("find_first_bit: empty returns 64", find_first_bit(arr, 64) == 64);
    TEST("find_first_bit: empty 128-bit", find_first_bit(arr, 128) == 128);

    /* 2. Single bit at position 0 */
    set_bit(0, arr);
    TEST("find_first_bit: bit 0", find_first_bit(arr, 128) == 0);
    clear_bit(0, arr);

    /* 3. Single bit at position 1 */
    set_bit(1, arr);
    TEST("find_first_bit: bit 1", find_first_bit(arr, 64) == 1);
    clear_bit(1, arr);

    /* 4. Single bit at position 63 */
    set_bit(63, arr);
    TEST("find_first_bit: bit 63", find_first_bit(arr, 64) == 63);
    clear_bit(63, arr);

    /* 5. Single bit in second word */
    set_bit(64, arr);
    TEST("find_first_bit: bit 64", find_first_bit(arr, 128) == 64);
    clear_bit(64, arr);

    /* 6. Multiple bits — returns lowest */
    set_bit(10, arr); set_bit(5, arr); set_bit(20, arr);
    TEST("find_first_bit: multiple returns lowest (5)", find_first_bit(arr, 128) == 5);

    /* 7. Bit at position 127 */
    unsigned long arr3[2] = {0, 0};
    set_bit(127, arr3);
    TEST("find_first_bit: bit 127", find_first_bit(arr3, 128) == 127);
}

/* ===================================================================
 *  test_find_first_zero_bit
 * =================================================================== */
static void test_find_first_zero_bit(void)
{
    unsigned long arr[2];
    memset(arr, 0xFF, sizeof(arr));  /* all bits set */

    /* 1. All set — returns size */
    TEST("find_first_zero_bit: all set returns 128", find_first_zero_bit(arr, 128) == 128);
    TEST("find_first_zero_bit: all set returns 64", find_first_zero_bit(arr, 64) == 64);

    /* 2. Zero at position 0 */
    clear_bit(0, arr);
    TEST("find_first_zero_bit: bit 0", find_first_zero_bit(arr, 128) == 0);
    set_bit(0, arr);

    /* 3. Zero at position 63 */
    clear_bit(63, arr);
    TEST("find_first_zero_bit: bit 63", find_first_zero_bit(arr, 128) == 63);
    set_bit(63, arr);

    /* 4. Zero in second word */
    clear_bit(100, arr);
    TEST("find_first_zero_bit: bit 100", find_first_zero_bit(arr, 128) == 100);
}

/* ===================================================================
 *  test_find_last_bit
 * =================================================================== */
static void test_find_last_bit(void)
{
    unsigned long arr[2] = {0, 0};

    /* 1. Empty — returns size (note: kernel returns 0 for size=0) */
    TEST("find_last_bit: empty", find_last_bit(arr, 64) == 64);

    /* 2. Single bit at 0 */
    set_bit(0, arr);
    TEST("find_last_bit: bit 0 only", find_last_bit(arr, 64) == 0);

    /* 3. Multiple bits */
    set_bit(10, arr);
    TEST("find_last_bit: bits 0,10", find_last_bit(arr, 64) == 10);

    /* 4. Bit at highest position */
    set_bit(63, arr);
    TEST("find_last_bit: highest is 63", find_last_bit(arr, 64) == 63);

    /* 5. Second word */
    set_bit(100, arr);
    TEST("find_last_bit: cross-word, highest 100", find_last_bit(arr, 128) == 100);

    /* 6. Only bit 0 */
    unsigned long arr2[2] = {0, 0};
    set_bit(0, arr2);
    TEST("find_last_bit: only bit 0", find_last_bit(arr2, 64) == 0);
}

/* ===================================================================
 *  test_find_next_bit
 * =================================================================== */
static void test_find_next_bit(void)
{
    unsigned long arr[2] = {0, 0};
    set_bit(5, arr);
    set_bit(10, arr);
    set_bit(70, arr);

    /* 1. Find from 0 */
    TEST("find_next_bit: offset 0 finds 5", find_next_bit(arr, 128, 0) == 5);

    /* 2. Find from after first */
    TEST("find_next_bit: offset 6 finds 10", find_next_bit(arr, 128, 6) == 10);

    /* 3. No more in first word */
    TEST("find_next_bit: offset 11 finds 70", find_next_bit(arr, 128, 11) == 70);

    /* 4. Past all bits */
    TEST("find_next_bit: past all returns size", find_next_bit(arr, 128, 71) == 128);

    /* 5. Empty */
    unsigned long e[2] = {0, 0};
    TEST("find_next_bit: empty", find_next_bit(e, 64, 0) == 64);

    /* 6. Offset equals position */
    TEST("find_next_bit: offset==pos", find_next_bit(arr, 128, 5) == 5);

    /* 7. Bit at position 127 */
    unsigned long arr3[2] = {0, 0};
    set_bit(127, arr3);
    TEST("find_next_bit: bit 127", find_next_bit(arr3, 128, 0) == 127);
}

/* ===================================================================
 *  test_find_next_zero_bit
 * =================================================================== */
static void test_find_next_zero_bit(void)
{
    unsigned long arr[2];
    memset(arr, 0xFF, sizeof(arr));
    clear_bit(10, arr);
    clear_bit(70, arr);

    /* 1. Find from 0 — first zero is at bit 10 */
    TEST("find_next_zero_bit: offset 0 finds 10", find_next_zero_bit(arr, 128, 0) == 10);

    /* 2. Find next in second word (bits 11-63 all set, first zero in word 2) */
    TEST("find_next_zero_bit: offset 11 finds 70", find_next_zero_bit(arr, 128, 11) == 70);

    /* 3. Past all zeros — size filled with all set bits to start */
    memset(arr, 0xFF, sizeof(arr));  /* all 1s, no zeros */
    TEST("find_next_zero_bit: all set returns size",
         find_next_zero_bit(arr, 128, 0) == 128);

    /* 4. Offset at first zero after reset */
    memset(arr, 0xFF, sizeof(arr));
    clear_bit(10, arr);
    TEST("find_next_zero_bit: offset at clear bit returns it",
         find_next_zero_bit(arr, 128, 10) == 10);

    /* 5. All set — no zeros (fresh memset) */
    unsigned long all[1];
    memset(all, 0xFF, sizeof(all));
    TEST("find_next_zero_bit: all set returns 64",
         find_next_zero_bit(all, 64, 0) == 64);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== find_bit Operations Tests ===\n\n");

    printf("--- set_bit / clear_bit / test_bit ---\n");
    test_set_clear_bit();

    printf("\n--- find_first_bit ---\n");
    test_find_first_bit();

    printf("\n--- find_first_zero_bit ---\n");
    test_find_first_zero_bit();

    printf("\n--- find_last_bit ---\n");
    test_find_last_bit();

    printf("\n--- find_next_bit ---\n");
    test_find_next_bit();

    printf("\n--- find_next_zero_bit ---\n");
    test_find_next_zero_bit();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
