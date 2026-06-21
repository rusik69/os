/*
 * test_bitfield.c — Host-side tests for kernel bitfield macros
 *
 * Tests BIT(), GENMASK(), FIELD_GET(), FIELD_PREP() macros
 * from src/include/bitfield.h. These are pure compile-time
 * macros so no external .o file is needed.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel bitfield macros (inlined from bitfield.h)
 * =================================================================== */
#define BIT(n)            (1ULL << (n))

#define GENMASK(h, l) \
    (((~0ULL) >> (63 - (h))) & ((~0ULL) << (l)))

#define FIELD_GET(mask, value) \
    (typeof(mask))((((typeof(value))(value)) & (mask)) >> __builtin_ctzll(mask))

#define FIELD_PREP(mask, value) \
    (((typeof(mask))(value) << __builtin_ctzll(mask)) & (mask))

/* ===================================================================
 *  Stubs (none needed, but linker expects them from some binaries)
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
 *  test_bit_macro
 * =================================================================== */
static void test_bit_macro(void)
{
    /* 1. BIT(0) == 1 */
    TEST("BIT(0) == 1", BIT(0) == 1);

    /* 2. BIT(1) == 2 */
    TEST("BIT(1) == 2", BIT(1) == 2);

    /* 3. BIT(31) == 0x80000000ULL */
    TEST("BIT(31) == 0x80000000", BIT(31) == 0x80000000ULL);

    /* 4. BIT(63) == 1ULL << 63 */
    TEST("BIT(63) == 1ULL<<63", BIT(63) == (1ULL << 63));

    /* 5. BIT(10) == 1024 */
    TEST("BIT(10) == 1024", BIT(10) == 1024);
}

/* ===================================================================
 *  test_genmask
 * =================================================================== */
static void test_genmask(void)
{
    /* 1. GENMASK(3, 0) == 0xF */
    TEST("GENMASK(3,0) == 0xF", GENMASK(3, 0) == 0xF);

    /* 2. GENMASK(7, 4) == 0xF0 */
    TEST("GENMASK(7,4) == 0xF0", GENMASK(7, 4) == 0xF0);

    /* 3. GENMASK(0, 0) == 1 */
    TEST("GENMASK(0,0) == 1", GENMASK(0, 0) == 1);

    /* 4. GENMASK(31, 0) == 0xFFFFFFFF */
    TEST("GENMASK(31,0) == 0xFFFFFFFF", GENMASK(31, 0) == 0xFFFFFFFFULL);

    /* 5. GENMASK(63, 63) == 1ULL << 63 */
    TEST("GENMASK(63,63) == BIT(63)", GENMASK(63, 63) == (1ULL << 63));

    /* 6. GENMASK(63, 0) == ~0ULL */
    TEST("GENMASK(63,0) == ~0ULL", GENMASK(63, 0) == ~0ULL);
}

/* ===================================================================
 *  test_field_get
 * =================================================================== */
static void test_field_get(void)
{
    /* 1. FIELD_GET(GENMASK(7,4), 0xF0) == 0xF */
    TEST("FIELD_GET(GENMASK(7,4), 0xF0) == 0xF",
         FIELD_GET(GENMASK(7, 4), 0xF0) == 0xF);

    /* 2. FIELD_GET(GENMASK(7,4), 0x0F) == 0 */
    TEST("FIELD_GET(GENMASK(7,4), 0x0F) == 0",
         FIELD_GET(GENMASK(7, 4), 0x0F) == 0);

    /* 3. FIELD_GET(GENMASK(3,0), 0xA) == 0xA */
    TEST("FIELD_GET(GENMASK(3,0), 0xA) == 0xA",
         FIELD_GET(GENMASK(3, 0), 0xA) == 0xA);

    /* 4. FIELD_GET(GENMASK(31,24), 0x12FFFFFF) == 0x12 */
    TEST("FIELD_GET(GENMASK(31,24), 0x12FFFFFF) == 0x12",
         FIELD_GET(GENMASK(31, 24), 0x12FFFFFF) == 0x12);

    /* 5. FIELD_GET(GENMASK(63,63), 0x8000000000000000ULL) == 1 */
    TEST("FIELD_GET(GENMASK(63,63), BIT(63)) == 1",
         FIELD_GET(GENMASK(63, 63), BIT(63)) == 1);

    /* 6. FIELD_GET on zero value returns 0 */
    TEST("FIELD_GET(mask, 0) == 0",
         FIELD_GET(GENMASK(7, 0), 0) == 0);
}

/* ===================================================================
 *  test_field_prep
 * =================================================================== */
static void test_field_prep(void)
{
    /* 1. FIELD_PREP(GENMASK(3,0), 0xA) == 0xA */
    TEST("FIELD_PREP(GENMASK(3,0), 0xA) == 0xA",
         FIELD_PREP(GENMASK(3, 0), 0xA) == 0xA);

    /* 2. FIELD_PREP(GENMASK(7,4), 0x5) == 0x50 */
    TEST("FIELD_PREP(GENMASK(7,4), 0x5) == 0x50",
         FIELD_PREP(GENMASK(7, 4), 0x5) == 0x50);

    /* 3. FIELD_PREP(GENMASK(31,24), 0xAB) == 0xAB000000 */
    TEST("FIELD_PREP(GENMASK(31,24), 0xAB) == 0xAB000000",
         FIELD_PREP(GENMASK(31, 24), 0xAB) == 0xAB000000ULL);

    /* 4. FIELD_PREP(GENMASK(63,63), 1) == BIT(63) */
    TEST("FIELD_PREP(GENMASK(63,63), 1) == BIT(63)",
         FIELD_PREP(GENMASK(63, 63), 1) == BIT(63));

    /* 5. FIELD_PREP(mask, 0) == 0 */
    TEST("FIELD_PREP(GENMASK(7,0), 0) == 0",
         FIELD_PREP(GENMASK(7, 0), 0) == 0);

    /* 6. FIELD_PREP + FIELD_GET roundtrip */
    TEST("FIELD_GET + FIELD_PREP roundtrip",
         FIELD_GET(GENMASK(15, 8), FIELD_PREP(GENMASK(15, 8), 0x42)) == 0x42);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Bitfield Macro Tests ===\n\n");

    printf("--- BIT() ---\n");
    test_bit_macro();

    printf("\n--- GENMASK() ---\n");
    test_genmask();

    printf("\n--- FIELD_GET() ---\n");
    test_field_get();

    printf("\n--- FIELD_PREP() ---\n");
    test_field_prep();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
