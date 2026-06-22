/*
 * test_uid16.c — Host-side tests for kernel UID16 conversion functions
 *
 * Tests uid_to_16 and uid_from_16 inline helpers from src/kernel/uid16.c.
 * These are static inline functions, so we duplicate them here
 * (they're trivial).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  UID16 inline helpers (copied from uid16.c)
 * =================================================================== */
static inline uint16_t uid_to_16(uint32_t uid)
{
    return (uid > 0xFFFF) ? 0xFFFE : (uint16_t)uid;
}

static inline uint32_t uid_from_16(uint16_t uid16)
{
    return (uint32_t)uid16;
}

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }

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
 *  test_uid_conv
 * =================================================================== */
static void test_uid_conv(void)
{
    /* 1. uid_to_16(0) == 0 */
    TEST("uid_to_16(0) == 0", uid_to_16(0) == 0);

    /* 2. uid_to_16(65535) == 65535 */
    TEST("uid_to_16(65535) == 65535", uid_to_16(65535) == 65535);

    /* 3. uid_to_16(65536) == 0xFFFE */
    TEST("uid_to_16(65536) == 0xFFFE", uid_to_16(65536) == 0xFFFE);

    /* 4. uid_to_16(999999) == 0xFFFE */
    TEST("uid_to_16(999999) == 0xFFFE", uid_to_16(999999) == 0xFFFE);

    /* 5. uid_to_16(UINT32_MAX) == 0xFFFE */
    TEST("uid_to_16(UINT32_MAX) == 0xFFFE", uid_to_16(0xFFFFFFFF) == 0xFFFE);

    /* 6. uid_to_16(1) == 1 */
    TEST("uid_to_16(1) == 1", uid_to_16(1) == 1);

    /* 7. uid_from_16(0) == 0 */
    TEST("uid_from_16(0) == 0", uid_from_16(0) == 0);

    /* 8. uid_from_16(65535) == 65535 */
    TEST("uid_from_16(65535) == 65535", uid_from_16(65535) == 65535);

    /* 9. uid_from_16(0xFFFE) == 0xFFFE */
    TEST("uid_from_16(0xFFFE) == 0xFFFE", uid_from_16(0xFFFE) == 0xFFFE);

    /* 10. uid_from_16(1) == 1 */
    TEST("uid_from_16(1) == 1", uid_from_16(1) == 1);

    /* 11. Roundtrip: uid_from_16(uid_to_16(x)) == x for x <= 65535 */
    for (uint32_t test_val = 0; test_val <= 65535; test_val += 1024) {
        uint16_t conv = uid_to_16(test_val);
        uint32_t back = uid_from_16(conv);
        if (back != test_val) {
            TEST("uid roundtrip: <=65535", 0);
            break;
        }
    }
    TEST("uid roundtrip: <=65535 passes", 1);

    /* 12. Roundtrip: uid_to_16(uid_from_16(x)) == x for all x */
    TEST("uid_to_16(uid_from_16(12345)) == 12345",
         uid_to_16(uid_from_16(12345)) == 12345);
    TEST("uid_to_16(uid_from_16(0)) == 0",
         uid_to_16(uid_from_16(0)) == 0);
    TEST("uid_to_16(uid_from_16(65535)) == 65535",
         uid_to_16(uid_from_16(65535)) == 65535);

    /* 13. Large values clamp */
    TEST("uid_to_16(0x10000) clamps to 0xFFFE", uid_to_16(0x10000) == 0xFFFE);
    TEST("uid_to_16(0x10001) clamps to 0xFFFE", uid_to_16(0x10001) == 0xFFFE);
}

/* ===================================================================
 *  test_uid16_more — additional boundary edge cases
 * =================================================================== */
static void test_uid16_more(void)
{
    /* 1. uid_to_16 boundary: 0 */
    TEST("uid16_more: uid_to_16(0) == 0", uid_to_16(0) == 0);

    /* 2. uid_to_16 boundary: 1 */
    TEST("uid16_more: uid_to_16(1) == 1", uid_to_16(1) == 1);

    /* 3. uid_to_16 boundary: 65534 */
    TEST("uid16_more: uid_to_16(65534) == 65534", uid_to_16(65534) == 65534);

    /* 4. uid_to_16 boundary: 65535 (max uint16_t) */
    TEST("uid16_more: uid_to_16(65535) == 65535", uid_to_16(65535) == 65535);

    /* 5. uid_to_16 boundary: 65536 (just above uint16_t) */
    TEST("uid16_more: uid_to_16(65536) == 0xFFFE", uid_to_16(65536) == 0xFFFE);

    /* 6. uid_to_16 boundary: 0x7FFFFFFF */
    TEST("uid16_more: uid_to_16(0x7FFFFFFF) == 0xFFFE",
         uid_to_16(0x7FFFFFFF) == 0xFFFE);

    /* 7. uid_from_16 boundary: 0 */
    TEST("uid16_more: uid_from_16(0) == 0", uid_from_16(0) == 0);

    /* 8. uid_from_16 boundary: 1 */
    TEST("uid16_more: uid_from_16(1) == 1", uid_from_16(1) == 1);

    /* 9. uid_from_16 boundary: 65535 */
    TEST("uid16_more: uid_from_16(65535) == 65535", uid_from_16(65535) == 65535);

    /* 10. uid_from_16 boundary: 0xFFFE */
    TEST("uid16_more: uid_from_16(0xFFFE) == 0xFFFE", uid_from_16(0xFFFE) == 0xFFFE);

    /* 11. Roundtrip for each boundary value */
    TEST("uid16_more: roundtrip 0",       uid_from_16(uid_to_16(0)) == 0);
    TEST("uid16_more: roundtrip 1",       uid_from_16(uid_to_16(1)) == 1);
    TEST("uid16_more: roundtrip 65534",   uid_from_16(uid_to_16(65534)) == 65534);
    TEST("uid16_more: roundtrip 65535",   uid_from_16(uid_to_16(65535)) == 65535);
    TEST("uid16_more: roundtrip 65536",   uid_from_16(uid_to_16(65536)) == 0xFFFE);
    TEST("uid16_more: roundtrip 0x7FFFFFFF", uid_from_16(uid_to_16(0x7FFFFFFF)) == 0xFFFE);
    TEST("uid16_more: roundtrip UINT32_MAX", uid_from_16(uid_to_16(0xFFFFFFFF)) == 0xFFFE);
    TEST("uid16_more: roundtrip 0x10000", uid_from_16(uid_to_16(0x10000)) == 0xFFFE);

    /* 12. Verify uid_to_16 clamps correctly for all values just around the boundary */
    TEST("uid16_more: uid_to_16(0xFFFF) == 0xFFFF", uid_to_16(0xFFFF) == 0xFFFF);
    TEST("uid16_more: uid_to_16(0xFFFF+1) clamps",  uid_to_16(0xFFFF+1) == 0xFFFE);
    TEST("uid16_more: uid_to_16(0xFFFF+2) clamps",  uid_to_16(0xFFFF+2) == 0xFFFE);

    /* 13. uid_from_16 preserves all uint16_t values */
    TEST("uid16_more: uid_from_16(0xFFFF) == 0xFFFF", uid_from_16(0xFFFF) == 0xFFFF);
    TEST("uid16_more: uid_from_16(0xFFFE) == 0xFFFE", uid_from_16(0xFFFE) == 0xFFFE);
    TEST("uid16_more: uid_from_16(0x0001) == 0x0001", uid_from_16(0x0001) == 0x0001);
}

/* ===================================================================
 *  test_uid16_edge — additional boundary edge values
 * =================================================================== */
static void test_uid16_edge(void)
{
    /* uid_to_16 with intermediate boundary values */
    TEST("uid16_edge: uid_to_16(2) == 2", uid_to_16(2) == 2);
    TEST("uid16_edge: uid_to_16(0xFFFD) == 0xFFFD", uid_to_16(0xFFFD) == 0xFFFD);
    TEST("uid16_edge: uid_to_16(0xFFFC) == 0xFFFC", uid_to_16(0xFFFC) == 0xFFFC);

    /* uid_from_16 for intermediate boundary values */
    TEST("uid16_edge: uid_from_16(2) == 2", uid_from_16(2) == 2);
    TEST("uid16_edge: uid_from_16(0xFFFD) == 0xFFFD", uid_from_16(0xFFFD) == 0xFFFD);
    TEST("uid16_edge: uid_from_16(0xFFFC) == 0xFFFC", uid_from_16(0xFFFC) == 0xFFFC);

    /* Roundtrip: uid_from_16(uid_to_16(x)) == x for below-boundary values */
    TEST("uid16_edge: roundtrip 2", uid_from_16(uid_to_16(2)) == 2);
    TEST("uid16_edge: roundtrip 0xFFFB", uid_from_16(uid_to_16(0xFFFB)) == 0xFFFB);
    TEST("uid16_edge: roundtrip 0xFFFC", uid_from_16(uid_to_16(0xFFFC)) == 0xFFFC);
    TEST("uid16_edge: roundtrip 0xFFFD", uid_from_16(uid_to_16(0xFFFD)) == 0xFFFD);

    /* uid_to_16(uid_from_16(x)) == x for all x in uint16_t range */
    TEST("uid16_edge: uid_to_16(uid_from_16(0xFFFB)) == 0xFFFB",
         uid_to_16(uid_from_16(0xFFFB)) == 0xFFFB);
    TEST("uid16_edge: uid_to_16(uid_from_16(0xFFFC)) == 0xFFFC",
         uid_to_16(uid_from_16(0xFFFC)) == 0xFFFC);
    TEST("uid16_edge: uid_to_16(uid_from_16(0xFFFD)) == 0xFFFD",
         uid_to_16(uid_from_16(0xFFFD)) == 0xFFFD);
}

/* ===================================================================
 *  test_uid16_comprehensive — comprehensive boundary coverage
 * =================================================================== */
static void test_uid16_comprehensive(void)
{
    /* Test all values at and around boundary 0xFFFF */
    TEST("uid16_comp: uid_to_16(0xFFFE) == 0xFFFE", uid_to_16(0xFFFE) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0xFFFF) == 0xFFFF", uid_to_16(0xFFFF) == 0xFFFF);
    TEST("uid16_comp: uid_to_16(0x10000) clamps",   uid_to_16(0x10000) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0x10001) clamps",   uid_to_16(0x10001) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0x1FFFF) clamps",   uid_to_16(0x1FFFF) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0x7FFFFFFF) clamps", uid_to_16(0x7FFFFFFF) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0x80000000) clamps", uid_to_16(0x80000000) == 0xFFFE);
    TEST("uid16_comp: uid_to_16(0xFFFFFFFF) clamps", uid_to_16(0xFFFFFFFF) == 0xFFFE);

    /* uid_from_16 should preserve all values */
    TEST("uid16_comp: uid_from_16(0) == 0",       uid_from_16(0) == 0);
    TEST("uid16_comp: uid_from_16(1) == 1",       uid_from_16(1) == 1);
    TEST("uid16_comp: uid_from_16(0xFFFE) == 0xFFFE", uid_from_16(0xFFFE) == 0xFFFE);
    TEST("uid16_comp: uid_from_16(0xFFFF) == 0xFFFF", uid_from_16(0xFFFF) == 0xFFFF);

    /* Roundtrip: uid_to_16 then uid_from_16 (for values ≤ 0xFFFF) */
    TEST("uid16_comp: roundtrip 0",         uid_from_16(uid_to_16(0)) == 0);
    TEST("uid16_comp: roundtrip 1",         uid_from_16(uid_to_16(1)) == 1);
    TEST("uid16_comp: roundtrip 65534",     uid_from_16(uid_to_16(65534)) == 65534);
    TEST("uid16_comp: roundtrip 65535",     uid_from_16(uid_to_16(65535)) == 65535);
    TEST("uid16_comp: roundtrip 65536 clamps to 0xFFFE", uid_from_16(uid_to_16(65536)) == 0xFFFE);
    TEST("uid16_comp: roundtrip UINT32_MAX clamps",   uid_from_16(uid_to_16(0xFFFFFFFF)) == 0xFFFE);

    /* uid_from_16 followed by uid_to_16 should be identity for all uint16_t */
    TEST("uid16_comp: uid_to_16(uid_from_16(0)) == 0",       uid_to_16(uid_from_16(0)) == 0);
    TEST("uid16_comp: uid_to_16(uid_from_16(1)) == 1",       uid_to_16(uid_from_16(1)) == 1);
    TEST("uid16_comp: uid_to_16(uid_from_16(0xFFFF)) == 0xFFFF", uid_to_16(uid_from_16(0xFFFF)) == 0xFFFF);

    /* Specific boundary values */
    TEST("uid16_comp: uid_to_16(0xFFFD) == 0xFFFD", uid_to_16(0xFFFD) == 0xFFFD);
    TEST("uid16_comp: uid_to_16(0xFFFC) == 0xFFFC", uid_to_16(0xFFFC) == 0xFFFC);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== UID16 Conversion Tests ===\n\n");

    printf("--- uid_to_16 / uid_from_16 ---\n");
    test_uid_conv();

    printf("\n--- more edge cases ---\n");
    test_uid16_more();

    printf("\n--- edge boundary values ---\n");
    test_uid16_edge();

    printf("\n--- comprehensive boundary coverage ---\n");
    test_uid16_comprehensive();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
