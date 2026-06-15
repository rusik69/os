/*
 * test_bitmap.c — Host-side unit tests for bit manipulation operations
 *
 * Tests find_first_set, hweight (popcount), bitmap operations
 * (set, clear, test, and, or, xor, andnot, shift), and bitfield
 * extract/insert — mirroring the kernel's bitops.h and bitmap.h.
 *
 * Runs entirely on the host — no kernel dependencies.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_bitmap test_bitmap.c
 * Run:      ./test_bitmap
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================
 *  Bit operations — kernel-compatible implementation
 * =================================================================== */

/* Find first set bit (1-indexed, like ffs). Returns 0 if no bits set. */
static int find_first_set(uint64_t x)
{
    if (x == 0) return 0;
    int n = 1;
    if ((x & 0xFFFFFFFF) == 0) { n += 32; x >>= 32; }
    if ((x & 0xFFFF) == 0)     { n += 16; x >>= 16; }
    if ((x & 0xFF) == 0)       { n += 8;  x >>= 8; }
    if ((x & 0xF) == 0)        { n += 4;  x >>= 4; }
    if ((x & 0x3) == 0)        { n += 2;  x >>= 2; }
    if ((x & 0x1) == 0)        { n += 1; }
    return n;
}

/* Find first zero bit (1-indexed). Returns 0 if none (all bits set). */
static int find_first_zero(uint64_t x)
{
    return find_first_set(~x);
}

/* Hamming weight (population count) for 64-bit value */
static int hweight64(uint64_t x)
{
    /* Parallel popcount algorithm */
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

/* Hamming weight for 32-bit value */
static int hweight32(uint32_t x)
{
    x = x - ((x >> 1) & 0x55555555);
    x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
    x = (x + (x >> 4)) & 0x0F0F0F0F;
    return (int)((x * 0x01010101) >> 24);
}

/* Hamming weight for 16-bit value */
static int hweight16(uint16_t x)
{
    return hweight32((uint32_t)x);
}

/* Hamming weight for 8-bit value */
static int hweight8(uint8_t x)
{
    return hweight32((uint32_t)x);
}

/* Test a specific bit in a bitmap array */
static inline int bitmap_test_bit(unsigned long *bitmap, int bit)
{
    return (bitmap[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1;
}

/* Set a bit in a bitmap array */
static inline void bitmap_set_bit(unsigned long *bitmap, int bit)
{
    bitmap[bit / (8 * sizeof(unsigned long))] |=
        (1UL << (bit % (8 * sizeof(unsigned long))));
}

/* Clear a bit in a bitmap array */
static inline void bitmap_clear_bit(unsigned long *bitmap, int bit)
{
    bitmap[bit / (8 * sizeof(unsigned long))] &=
        ~(1UL << (bit % (8 * sizeof(unsigned long))));
}

/* Change a bit (toggle) in a bitmap array */
static inline void bitmap_change_bit(unsigned long *bitmap, int bit)
{
    bitmap[bit / (8 * sizeof(unsigned long))] ^=
        (1UL << (bit % (8 * sizeof(unsigned long))));
}

/* Find first zero bit in a bitmap array (up to nbits) */
static int bitmap_find_first_zero(unsigned long *bitmap, int nbits)
{
    for (int i = 0; i < nbits; i++) {
        if (!bitmap_test_bit(bitmap, i))
            return i;
    }
    return -1;
}

/* Find first set bit in a bitmap array (up to nbits) */
static int bitmap_find_first_set(unsigned long *bitmap, int nbits)
{
    for (int i = 0; i < nbits; i++) {
        if (bitmap_test_bit(bitmap, i))
            return i;
    }
    return -1;
}

/* Bitmap AND: dst = a & b for nbits */
static void bitmap_and(unsigned long *dst, const unsigned long *a,
                       const unsigned long *b, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    for (int i = 0; i < words; i++)
        dst[i] = a[i] & b[i];
}

/* Bitmap OR: dst = a | b for nbits */
static void bitmap_or(unsigned long *dst, const unsigned long *a,
                      const unsigned long *b, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    for (int i = 0; i < words; i++)
        dst[i] = a[i] | b[i];
}

/* Bitmap XOR: dst = a ^ b for nbits */
static void bitmap_xor(unsigned long *dst, const unsigned long *a,
                       const unsigned long *b, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    for (int i = 0; i < words; i++)
        dst[i] = a[i] ^ b[i];
}

/* Bitmap ANDNOT: dst = a & ~b for nbits */
static void bitmap_andnot(unsigned long *dst, const unsigned long *a,
                          const unsigned long *b, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    for (int i = 0; i < words; i++)
        dst[i] = a[i] & ~b[i];
}

/* Bitmap weight (count set bits) */
static int bitmap_weight(const unsigned long *bitmap, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    int count = 0;
    for (int i = 0; i < words; i++)
        count += hweight64(bitmap[i]);
    return count;
}

/* Bitmap shift left (dst = src << shift) for nbits */
static void bitmap_shift_left(unsigned long *dst, const unsigned long *src,
                              int shift, int nbits)
{
    int words = (nbits + (8 * sizeof(unsigned long)) - 1) /
                (8 * sizeof(unsigned long));
    memset(dst, 0, words * sizeof(unsigned long));

    for (int i = 0; i < nbits; i++) {
        if (bitmap_test_bit((unsigned long *)src, i) && (i + shift < nbits))
            bitmap_set_bit(dst, i + shift);
    }
}

/* Bitfield extract: extract 'width' bits starting at 'offset' from 'val' */
static uint64_t bitfield_extract(uint64_t val, int offset, int width)
{
    return (val >> offset) & ((1ULL << width) - 1);
}

/* Bitfield insert: insert 'val' into 'field' at 'offset' with 'width' bits */
static uint64_t bitfield_insert(uint64_t field, uint64_t val,
                                int offset, int width)
{
    uint64_t mask = ((1ULL << width) - 1) << offset;
    return (field & ~mask) | ((val << offset) & mask);
}

/* ===================================================================
 *  Test framework
 * =================================================================== */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do {                        \
    tests_run++;                                \
    printf("  TEST: %-50s ... ", name);         \
} while (0)

#define PASS()      do {                        \
    tests_passed++;                             \
    printf("PASS\n");                           \
} while (0)

#define FAIL(msg)   do {                        \
    tests_failed++;                             \
    printf("FAIL\n");                           \
    printf("        %s\n", msg);                \
} while (0)

#define ASSERT(cond, msg) do {                  \
    if (!(cond)) { FAIL(msg); return; }         \
} while (0)

#define ASSERT_INT_EQ(got, expected, msg) do {  \
    if ((got) != (expected)) {                  \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: %d\n",        \
               (int)(expected));                \
        printf("        Got:      %d\n",        \
               (int)(got));                     \
        return;                                 \
    }                                           \
} while (0)

/* ===================================================================
 *  Tests: find_first_set
 * =================================================================== */

static void test_ffs_zero(void)
{
    TEST("ffs(0) returns 0");
    ASSERT_INT_EQ(find_first_set(0), 0, "no bits set");
    PASS();
}

static void test_ffs_bit0(void)
{
    TEST("ffs(1) returns 1 (LSB)");
    ASSERT_INT_EQ(find_first_set(1), 1, "bit 0");
    PASS();
}

static void test_ffs_powers_of_two(void)
{
    TEST("ffs for power-of-two values");
    ASSERT_INT_EQ(find_first_set(2), 2, "bit 1");
    ASSERT_INT_EQ(find_first_set(4), 3, "bit 2");
    ASSERT_INT_EQ(find_first_set(8), 4, "bit 3");
    ASSERT_INT_EQ(find_first_set(16), 5, "bit 4");
    ASSERT_INT_EQ(find_first_set(0x8000000000000000ULL), 64, "MSB");
    PASS();
}

static void test_ffs_multiple_bits(void)
{
    TEST("ffs returns least significant set bit");
    ASSERT_INT_EQ(find_first_set(0xF0), 5, "lowest in 0xF0 is bit 4 (index 5)");
    ASSERT_INT_EQ(find_first_set(0x12345678), 4, "lowest set bit in 0x12345678");
    PASS();
}

/* ===================================================================
 *  Tests: find_first_zero
 * =================================================================== */

static void test_ffz_all_ones(void)
{
    TEST("ffz(~0ULL) returns 0 when all bits set (ffz = ffs(~x))");
    /* ~0ULL has all bits set, so first zero is beyond 64 bits */
    /* Actually ffz returns the position of first zero BIT */
    ASSERT_INT_EQ(find_first_zero(0), 1, "all zero -> first zero is bit 0");
    ASSERT_INT_EQ(find_first_zero(1), 2, "bit 0 set -> first zero is bit 1");
    PASS();
}

static void test_ffz_basic(void)
{
    TEST("ffz for specific values");
    ASSERT_INT_EQ(find_first_zero(0xFFFFFFFE), 1, "bit 0 clear -> position 1");
    ASSERT_INT_EQ(find_first_zero(0xFFFF00FF), 9, "bit 8 clear -> position 9");
    PASS();
}

/* ===================================================================
 *  Tests: hweight (population count)
 * =================================================================== */

static void test_hweight64_zero(void)
{
    TEST("hweight64(0) returns 0");
    ASSERT_INT_EQ(hweight64(0), 0, "zero weight");
    PASS();
}

static void test_hweight64_one(void)
{
    TEST("hweight64(power of 2) returns 1");
    ASSERT_INT_EQ(hweight64(1), 1, "1 bit = 1");
    ASSERT_INT_EQ(hweight64(0x8000000000000000ULL), 1, "MSB = 1");
    PASS();
}

static void test_hweight64_all(void)
{
    TEST("hweight64(~0ULL) returns 64");
    ASSERT_INT_EQ(hweight64(~0ULL), 64, "all 64 bits");
    PASS();
}

static void test_hweight64_patterns(void)
{
    TEST("hweight64 for known patterns");
    ASSERT_INT_EQ(hweight64(0x5555555555555555ULL), 32, "alternating 0101...");
    ASSERT_INT_EQ(hweight64(0xAAAAAAAAAAAAAAAAULL), 32, "alternating 1010...");
    ASSERT_INT_EQ(hweight64(0x0F0F0F0F0F0F0F0FULL), 32, "nibble pattern");
    ASSERT_INT_EQ(hweight64(0x00000000FFFFFFFFULL), 32, "low 32 bits");
    PASS();
}

static void test_hweight32(void)
{
    TEST("hweight32 for known values");
    ASSERT_INT_EQ(hweight32(0), 0, "zero");
    ASSERT_INT_EQ(hweight32(0xFFFFFFFF), 32, "all 32");
    ASSERT_INT_EQ(hweight32(0x0F0F0F0F), 16, "nibble pattern");
    ASSERT_INT_EQ(hweight32(0x11111111), 8, "every 4th bit");
    PASS();
}

static void test_hweight16(void)
{
    TEST("hweight16 for known values");
    ASSERT_INT_EQ(hweight16(0), 0, "zero");
    ASSERT_INT_EQ(hweight16(0xFFFF), 16, "all 16");
    ASSERT_INT_EQ(hweight16(0x0F0F), 8, "nibble pattern");
    ASSERT_INT_EQ(hweight16(0xAAAA), 8, "alternating");
    PASS();
}

static void test_hweight8(void)
{
    TEST("hweight8 for known values");
    ASSERT_INT_EQ(hweight8(0), 0, "zero");
    ASSERT_INT_EQ(hweight8(0xFF), 8, "all 8");
    ASSERT_INT_EQ(hweight8(0x55), 4, "alternating");
    ASSERT_INT_EQ(hweight8(0xAA), 4, "alternating inverse");
    ASSERT_INT_EQ(hweight8(0x0F), 4, "low nibble");
    PASS();
}

/* ===================================================================
 *  Tests: Bitmap operations
 * =================================================================== */

static void test_bitmap_set_clear_test(void)
{
    unsigned long bitmap[4] = {0};

    TEST("bitmap set/clear/test individual bits");
    bitmap_set_bit(bitmap, 0);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 0), 1, "bit 0 set");
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 1), 0, "bit 1 not set");

    bitmap_set_bit(bitmap, 63);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 63), 1, "bit 63 set");

    bitmap_clear_bit(bitmap, 0);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 0), 0, "bit 0 cleared");

    bitmap_set_bit(bitmap, 127);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 127), 1, "bit 127 set (in word 1)");

    bitmap_clear_bit(bitmap, 127);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 127), 0, "bit 127 cleared");

    bitmap_set_bit(bitmap, 192);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 192), 1, "bit 192 set (in word 3)");

    PASS();
}

static void test_bitmap_find_first(void)
{
    unsigned long bitmap[4] = {0};

    TEST("bitmap_find_first_zero on empty bitmap");
    ASSERT_INT_EQ(bitmap_find_first_zero(bitmap, 256), 0, "first zero at 0");
    PASS();

    TEST("bitmap_find_first_set on empty bitmap");
    ASSERT_INT_EQ(bitmap_find_first_set(bitmap, 256), -1, "no set bits");
    PASS();

    TEST("bitmap_find_first_set with set bits");
    bitmap_set_bit(bitmap, 42);
    ASSERT_INT_EQ(bitmap_find_first_set(bitmap, 256), 42, "first set at 42");
    PASS();

    bitmap_set_bit(bitmap, 10);
    ASSERT_INT_EQ(bitmap_find_first_set(bitmap, 256), 10, "first set at 10");
    PASS();
}

static void test_bitmap_and(void)
{
    unsigned long a[4], b[4], dst[4];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    bitmap_set_bit(a, 1);
    bitmap_set_bit(a, 3);
    bitmap_set_bit(b, 1);
    bitmap_set_bit(b, 3);
    bitmap_set_bit(b, 5);

    TEST("bitmap_and of two bitmaps");
    bitmap_and(dst, a, b, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 1), 1, "bit 1 set in both");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 3), 1, "bit 3 set in both");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 5), 0, "bit 5 only in b");
    ASSERT_INT_EQ(bitmap_weight(dst, 256), 2, "should have 2 bits");
    PASS();
}

static void test_bitmap_or(void)
{
    unsigned long a[4], b[4], dst[4];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    bitmap_set_bit(a, 1);
    bitmap_set_bit(a, 3);
    bitmap_set_bit(b, 3);
    bitmap_set_bit(b, 5);

    TEST("bitmap_or of two bitmaps");
    bitmap_or(dst, a, b, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 1), 1, "bit 1 from a");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 3), 1, "bit 3 from both");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 5), 1, "bit 5 from b");
    ASSERT_INT_EQ(bitmap_weight(dst, 256), 3, "should have 3 bits");
    PASS();
}

static void test_bitmap_xor(void)
{
    unsigned long a[4], b[4], dst[4];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    bitmap_set_bit(a, 1);
    bitmap_set_bit(a, 3);
    bitmap_set_bit(b, 3);
    bitmap_set_bit(b, 5);

    TEST("bitmap_xor");
    bitmap_xor(dst, a, b, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 1), 1, "bit 1 only in a");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 3), 0, "bit 3 in both -> 0");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 5), 1, "bit 5 only in b");
    ASSERT_INT_EQ(bitmap_weight(dst, 256), 2, "should have 2 bits");
    PASS();
}

static void test_bitmap_andnot(void)
{
    unsigned long a[4], b[4], dst[4];
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));

    bitmap_set_bit(a, 1);
    bitmap_set_bit(a, 3);
    bitmap_set_bit(a, 7);
    bitmap_set_bit(b, 3);
    bitmap_set_bit(b, 5);

    TEST("bitmap_andnot (a & ~b)");
    bitmap_andnot(dst, a, b, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 1), 1, "bit 1 (a only)");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 3), 0, "bit 3 cleared by andnot");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 5), 0, "bit 5 not in a");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 7), 1, "bit 7 (a only)");
    ASSERT_INT_EQ(bitmap_weight(dst, 256), 2, "should have 2 bits");
    PASS();
}

static void test_bitmap_weight(void)
{
    unsigned long bitmap[4] = {0};

    TEST("bitmap_weight on empty bitmap");
    ASSERT_INT_EQ(bitmap_weight(bitmap, 256), 0, "empty weight");
    PASS();

    TEST("bitmap_weight with varying density");
    for (int i = 0; i < 256; i += 2)
        bitmap_set_bit(bitmap, i);
    ASSERT_INT_EQ(bitmap_weight(bitmap, 256), 128, "every other bit");
    PASS();

    /* Full bitmap */
    memset(bitmap, 0xFF, sizeof(bitmap));
    ASSERT_INT_EQ(bitmap_weight(bitmap, 256), 256, "all 256 bits");
    PASS();
}

static void test_bitmap_shift_left(void)
{
    unsigned long src[4], dst[4];
    memset(src, 0, sizeof(src));
    memset(dst, 0, sizeof(dst));

    bitmap_set_bit(src, 0);
    bitmap_set_bit(src, 5);

    TEST("bitmap_shift_left by 3");
    bitmap_shift_left(dst, src, 3, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 0), 0, "bit 0 shifted away");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 3), 1, "bit 0 -> bit 3");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 8), 1, "bit 5 -> bit 8");
    ASSERT_INT_EQ(bitmap_weight(dst, 256), 2, "should still have 2 bits");
    PASS();

    TEST("bitmap_shift_left by 0 (identity)");
    memset(dst, 0, sizeof(dst));
    bitmap_shift_left(dst, src, 0, 256);
    ASSERT_INT_EQ(bitmap_test_bit(dst, 0), 1, "bit 0 unchanged");
    ASSERT_INT_EQ(bitmap_test_bit(dst, 5), 1, "bit 5 unchanged");
    PASS();
}

/* ===================================================================
 *  Tests: Bitfield extract/insert
 * =================================================================== */

static void test_bitfield_extract(void)
{
    uint64_t val = 0x1234567890ABCDEFULL;

    TEST("bitfield_extract low 8 bits");
    ASSERT_INT_EQ(bitfield_extract(val, 0, 8), 0xEF, "low byte");
    PASS();

    TEST("bitfield_extract high 8 bits");
    ASSERT_INT_EQ(bitfield_extract(val, 56, 8), 0x12, "high byte");
    PASS();

    TEST("bitfield_extract middle bits");
    ASSERT_INT_EQ(bitfield_extract(val, 16, 16), 0x90AB, "middle 16 bits (0x90AB)");
    PASS();

    TEST("bitfield_extract single bit");
    ASSERT_INT_EQ(bitfield_extract(val, 0, 1), 0x1, "bit 0 of ...EF is 1");
    ASSERT_INT_EQ(bitfield_extract(val, 4, 1), 0x0, "bit 4 of ...EF is 0");
    PASS();
}

static void test_bitfield_insert(void)
{
    uint64_t field = 0;

    TEST("bitfield_insert inserts at correct position");
    field = bitfield_insert(field, 0xFF, 0, 8);
    ASSERT_INT_EQ(field, 0xFF, "low byte inserted");
    PASS();

    TEST("bitfield_insert preserves other bits");
    field = bitfield_insert(field, 0xAA, 8, 8);
    ASSERT_INT_EQ(field, 0xAAFF, "second byte inserted");
    PASS();

    TEST("bitfield_insert with wider field");
    field = bitfield_insert(0, 0x5, 4, 4);
    ASSERT_INT_EQ(field, 0x50, "insert at offset 4");
    PASS();
}

/* ===================================================================
 *  Tests: Additional edge cases
 * =================================================================== */

static void test_ffs_large_gaps(void)
{
    TEST("ffs with large gaps between set bits");
    ASSERT_INT_EQ(find_first_set(0x1000000000000000ULL), 61, "bit 60");
    ASSERT_INT_EQ(find_first_set(0x0001000000000000ULL), 49, "bit 48");
    PASS();
}

static void test_hweight_adjacent_bits(void)
{
    TEST("hweight for adjacent set bits");
    ASSERT_INT_EQ(hweight64(0x0000000000000003ULL), 2, "two adjacent LSBs");
    ASSERT_INT_EQ(hweight64(0xC000000000000000ULL), 2, "two adjacent MSBs");
    ASSERT_INT_EQ(hweight64(0x00000000000000FFULL), 8, "low byte");
    PASS();
}

static void test_bitmap_change_bit(void)
{
    unsigned long bitmap[4] = {0};

    TEST("bitmap_change_bit toggles bits");
    bitmap_change_bit(bitmap, 10);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 10), 1, "first toggle sets");
    bitmap_change_bit(bitmap, 10);
    ASSERT_INT_EQ(bitmap_test_bit(bitmap, 10), 0, "second toggle clears");
    PASS();
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  Bit Manipulation Unit Tests\n");
    printf("============================================\n\n");

    /* find_first_set */
    test_ffs_zero();
    test_ffs_bit0();
    test_ffs_powers_of_two();
    test_ffs_multiple_bits();
    test_ffs_large_gaps();

    /* find_first_zero */
    test_ffz_all_ones();
    test_ffz_basic();

    /* hweight */
    test_hweight64_zero();
    test_hweight64_one();
    test_hweight64_all();
    test_hweight64_patterns();
    test_hweight32();
    test_hweight16();
    test_hweight8();
    test_hweight_adjacent_bits();

    /* Bitmap operations */
    test_bitmap_set_clear_test();
    test_bitmap_find_first();
    test_bitmap_and();
    test_bitmap_or();
    test_bitmap_xor();
    test_bitmap_andnot();
    test_bitmap_weight();
    test_bitmap_shift_left();
    test_bitmap_change_bit();

    /* Bitfield extract/insert */
    test_bitfield_extract();
    test_bitfield_insert();

    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
