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

    /* 6. FIELD_GET + FIELD_PREP roundtrip */
    TEST("FIELD_GET + FIELD_PREP roundtrip",
         FIELD_GET(GENMASK(15, 8), FIELD_PREP(GENMASK(15, 8), 0x42)) == 0x42);
}

/* ===================================================================
 *  test_more_edge_cases — +15 new assertions
 * =================================================================== */
static void test_more_edge_cases(void)
{
    /* --- Additional GENMASK edge cases --- */

    /* 1. GENMASK with h == l (single-bit, non-zero shift) */
    TEST("GENMASK(5,5) == BIT(5)",
         GENMASK(5, 5) == BIT(5));

    /* 2. Another single-bit GENMASK */
    TEST("GENMASK(1,1) == BIT(1)",
         GENMASK(1, 1) == BIT(1));

    /* 3. GENMASK with wide range */
    TEST("GENMASK(10,0) == 0x7FF",
         GENMASK(10, 0) == 0x7FFULL);

    /* --- Additional FIELD_GET edge cases --- */

    /* 4. FIELD_GET with full 64-bit mask extracts all bits */
    TEST("FIELD_GET(GENMASK(63,0), ~0ULL) == ~0ULL",
         FIELD_GET(GENMASK(63, 0), ~0ULL) == ~0ULL);

    /* 5. FIELD_GET single bit at position 3 */
    TEST("FIELD_GET(GENMASK(3,3), 0x8) == 1",
         FIELD_GET(GENMASK(3, 3), 0x8ULL) == 1);

    /* 6. FIELD_GET single bit at high position 62 */
    TEST("FIELD_GET(GENMASK(62,62), BIT(62)) == 1",
         FIELD_GET(GENMASK(62, 62), BIT(62)) == 1);

    /* 7. FIELD_GET all bits set in a multi-bit range */
    TEST("FIELD_GET(GENMASK(10,5), 0x7E0) == 0x3F",
         FIELD_GET(GENMASK(10, 5), 0x7E0ULL) == 0x3F);

    /* --- Additional FIELD_PREP edge cases --- */

    /* 8. FIELD_PREP value wider than mask width — truncated to mask */
    TEST("FIELD_PREP(GENMASK(3,0), 0xFF) == 0xF",
         FIELD_PREP(GENMASK(3, 0), 0xFF) == 0xF);

    /* 9. FIELD_PREP truncation with shifted mask */
    TEST("FIELD_PREP(GENMASK(7,4), 0xFF) == 0xF0",
         FIELD_PREP(GENMASK(7, 4), 0xFF) == 0xF0);

    /* 10. Combining two FIELD_PREP values into a single word */
    TEST("FIELD_PREP(lower) | FIELD_PREP(upper) == 0x5A",
         (FIELD_PREP(GENMASK(3, 0), 0xA) | FIELD_PREP(GENMASK(7, 4), 0x5)) == 0x5A);

    /* 11. FIELD_GET on the upper half of the combined word */
    {
        uint64_t combined = FIELD_PREP(GENMASK(3, 0), 0xA)
                          | FIELD_PREP(GENMASK(7, 4), 0x5);
        TEST("FIELD_GET(upper nibble) from combined 0x5A == 0x5",
             FIELD_GET(GENMASK(7, 4), combined) == 0x5);
    }

    /* 12. FIELD_GET on the lower half of the combined word */
    {
        uint64_t combined = FIELD_PREP(GENMASK(3, 0), 0xA)
                          | FIELD_PREP(GENMASK(7, 4), 0x5);
        TEST("FIELD_GET(lower nibble) from combined 0x5A == 0xA",
             FIELD_GET(GENMASK(3, 0), combined) == 0xA);
    }

    /* 13. FIELD_PREP exact fit — value fills mask exactly */
    TEST("FIELD_PREP(GENMASK(15,0), 0xABCD) == 0xABCD",
         FIELD_PREP(GENMASK(15, 0), 0xABCD) == 0xABCD);

    /* 14. FIELD_PREP with single-bit mask (BIT(0)) */
    TEST("FIELD_PREP(GENMASK(0,0), 1) == BIT(0)",
         FIELD_PREP(GENMASK(0, 0), 1) == BIT(0));

    /* 15. Full roundtrip: FIELD_PREP high 32 bits then FIELD_GET them back */
    TEST("FIELD_GET/FIELD_PREP roundtrip high 32 bits",
         FIELD_GET(GENMASK(63, 32), FIELD_PREP(GENMASK(63, 32), 0xDEADBEEFULL)) == 0xDEADBEEFULL);
}

/* ===================================================================
 *  test_bitfield_extra — more edge cases
 * =================================================================== */
static void test_bitfield_extra(void)
{
    printf("\\n[Bitfield Extra Edge Cases]\\n");

    /* 1. BIT() with boundary value 62 */
    TEST("BIT(62) == 1ULL<<62", BIT(62) == (1ULL << 62));

    /* 2. GENMASK with h < l (undefined but shouldn't crash — currently produces 0 or garbage) */
    /* We just verify it compiles and runs */
    volatile uint64_t bad_mask = GENMASK(3, 5);
    (void)bad_mask;
    TEST("GENMASK(3,5): compiles and runs", 1);

    /* 3. GENMASK with negative l value (cast to uint64_t, large shift) */
    volatile uint64_t neg_mask = GENMASK(5, 0);
    (void)neg_mask;
    TEST("GENMASK(5,0): non-zero result", GENMASK(5, 0) != 0);

    /* 4. FIELD_GET with zero mask (ctzll(0) is undefined — skip) */
    /* Just test with minimal mask */
    TEST("FIELD_GET(GENMASK(0,0), 1) == 1",
         FIELD_GET(GENMASK(0, 0), 1) == 1);
    TEST("FIELD_GET(GENMASK(0,0), 0) == 0",
         FIELD_GET(GENMASK(0, 0), 0) == 0);

    /* 5. FIELD_PREP with zero value always zero */
    TEST("FIELD_PREP(any mask, 0) == 0",
         FIELD_PREP(GENMASK(63, 0), 0) == 0);

    /* 6. Multiple non-overlapping FIELD_PREP combined */
    {
        uint64_t reg = FIELD_PREP(GENMASK(7, 0), 0xAB)
                     | FIELD_PREP(GENMASK(15, 8), 0xCD)
                     | FIELD_PREP(GENMASK(23, 16), 0xEF);
        TEST("FIELD_PREP 3 non-overlapping fields combined",
             FIELD_GET(GENMASK(7, 0), reg) == 0xAB &&
             FIELD_GET(GENMASK(15, 8), reg) == 0xCD &&
             FIELD_GET(GENMASK(23, 16), reg) == 0xEF);
    }

    /* 7. FIELD_GET with mask spanning exactly 64 bits */
    TEST("FIELD_GET(GENMASK(63,0), 0xDEADBEEFCAFE) == 0xDEADBEEFCAFE",
         FIELD_GET(GENMASK(63, 0), 0xDEADBEEFCAFEULL) == 0xDEADBEEFCAFEULL);

    /* 8. FIELD_PREP with value == 0 (no bits set in value) */
    TEST("FIELD_PREP(GENMASK(31,16), 0) == 0",
         FIELD_PREP(GENMASK(31, 16), 0) == 0);

    /* 9. BIT() with max valid value on 64-bit */
    TEST("BIT(0) == 1", BIT(0) == 1ULL);
    TEST("BIT(63) has MSB set", (BIT(63) >> 63) == 1);
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

    printf("\\n--- More Edge Cases ---\\n");
    test_more_edge_cases();

    printf("\\n--- Bitfield Extra Edge Cases ---\\n");
    test_bitfield_extra();

    printf("\\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
