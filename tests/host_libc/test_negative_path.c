/*
 * test_negative_path.c — Host-side edge-case / negative-path tests
 *
 * Tests four categories:
 *   1. String edge cases   (strlcpy, strlcat, strtrim, strtol)
 *   2. Memory edge cases   (memset, memcpy, memcmp variants)
 *   3. Bitfield operations  (BIT, GENMASK, FIELD_GET, FIELD_PREP)
 *   4. Negative-path tests  (NULL ptrs, zero-size, boundary values)
 *
 * Self-contained — uses system libc and inline bitfield macros.
 * No kernel source dependencies.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_negative_path test_negative_path.c
 * Run:      ./test_negative_path
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>

/* ===================================================================
 *  strtrim implementation (simplified kernel equivalent)
 * =================================================================== */
static char *strtrim_local(char *s) {
    if (!s) return s;
    /* Skip leading whitespace */
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    /* Remove trailing whitespace */
    int len = (int)strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    return s;
}

/* ===================================================================
 *  Bitfield macros (from src/include/bitfield.h)
 * =================================================================== */
#define BIT(n)            (1ULL << (n))

#define GENMASK(h, l) \
    (((~0ULL) >> (63 - (h))) & ((~0ULL) << (l)))

#define FIELD_GET(mask, value) \
    (typeof(mask))((((typeof(value))(value)) & (mask)) >> __builtin_ctzll(mask))

#define FIELD_PREP(mask, value) \
    (((typeof(mask))(value) << __builtin_ctzll(mask)) & (mask))

/* ===================================================================
 *  Test framework
 * =================================================================== */
static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do {                        \
    tests_run++;                                \
    printf("  TEST: %-55s ... ", name);         \
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
        printf("        Expected: %ld\n",       \
               (long)(expected));              \
        printf("        Got:      %ld\n",       \
               (long)(got));                   \
        return;                                 \
    }                                           \
} while (0)

#define ASSERT_LONG_EQ(got, expected, msg) do { \
    if ((got) != (expected)) {                  \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: %ld\n",       \
               (long)(expected));              \
        printf("        Got:      %ld\n",       \
               (long)(got));                   \
        return;                                 \
    }                                           \
} while (0)

#define ASSERT_STR_EQ(got, expected, msg) do {  \
    if (strcmp((got), (expected)) != 0) {       \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected: \"%s\"\n",     \
               (expected));                     \
        printf("        Got:      \"%s\"\n",     \
               (got));                          \
        return;                                 \
    }                                           \
} while (0)

/* ===================================================================
 *  CATEGORY 1: String edge cases
 *
 *  Tests: strlcpy (zero-length dst), strlcat (full buffer),
 *         strtrim (all-whitespace, empty), strtol (overflow,
 *         negative, leading whitespace)
 * =================================================================== */

static void test_strlcpy_zero_dst(void)
{
    /* strlcpy with size=0 must NOT write to dst, returns src length */
    TEST("strlcpy zero-length dst returns src length");
    {
        char buf[16] = "unchanged";
        size_t ret = strlcpy(buf, "hello", 0);
        ASSERT_INT_EQ((int)ret, 5, "returns strlen(src)=5");
        ASSERT_STR_EQ(buf, "unchanged", "buffer unchanged");
    }
    PASS();

    TEST("strlcpy size=1 dst (only null terminator fits)");
    {
        char buf[4] = "XYZ";
        size_t ret = strlcpy(buf, "abc", 1);
        ASSERT_INT_EQ((int)ret, 3, "returns strlen(src)=3");
        ASSERT_INT_EQ((unsigned char)buf[0], 0, "buf[0] = '\\0'");
    }
    PASS();

    TEST("strlcpy size=2 dst (one char + null)");
    {
        char buf[4] = "XYZ";
        size_t ret = strlcpy(buf, "abc", 2);
        ASSERT_INT_EQ((int)ret, 3, "returns strlen(src)=3");
        ASSERT_STR_EQ(buf, "a", "copies 1 char + null");
    }
    PASS();
}

static void test_strlcat_full_buffer(void)
{
    TEST("strlcat empty src to full buffer");
    {
        char buf[4] = "abc";
        size_t ret = strlcat(buf, "", sizeof(buf));
        ASSERT_INT_EQ((int)ret, 3, "returns strlen(dst)=3");
        ASSERT_STR_EQ(buf, "abc", "buffer unchanged");
    }
    PASS();

    TEST("strlcat to buffer with no space (dst fills exactly)");
    {
        char buf[8] = "1234567";
        size_t ret = strlcat(buf, "X", sizeof(buf));
        ASSERT_INT_EQ((int)ret, 8, "return = dlen+slen = 7+1 = 8");
        ASSERT_STR_EQ(buf, "1234567", "no bytes appended");
    }
    PASS();

    TEST("strlcat full buffer with truncation");
    {
        char buf[6] = "abcd";
        size_t ret = strlcat(buf, "efgh", sizeof(buf));
        ASSERT_INT_EQ((int)ret, 8, "return = 4+4 = 8 (theoretical)");
        ASSERT_STR_EQ(buf, "abcde", "truncated with null");
    }
    PASS();
}

static void test_strtrim_all_whitespace(void)
{
    TEST("strtrim all whitespace becomes empty");
    {
        char s[] = "   \t\n\r\f\v   ";
        ASSERT_STR_EQ(strtrim_local(s), "", "all whitespace stripped");
    }
    PASS();

    TEST("strtrim all tabs and spaces");
    {
        char s[] = "\t\t   \t\t";
        ASSERT_STR_EQ(strtrim_local(s), "", "tabs and spaces stripped");
    }
    PASS();
}

static void test_strtrim_empty(void)
{
    TEST("strtrim empty string");
    {
        char s[] = "";
        ASSERT_STR_EQ(strtrim_local(s), "", "empty stays empty");
    }
    PASS();

    TEST("strtrim single-char string with no whitespace");
    {
        char s[] = "x";
        ASSERT_STR_EQ(strtrim_local(s), "x", "single char unchanged");
    }
    PASS();
}

static void test_strtol_overflow(void)
{
    char *endptr;

    TEST("strtol overflow returns LONG_MAX");
    {
        long val = strtol("999999999999999999999999999", &endptr, 10);
        ASSERT_LONG_EQ(val, LONG_MAX, "overflow clamps to LONG_MAX");
    }
    PASS();

    TEST("strtol negative overflow returns LONG_MIN");
    {
        long val = strtol("-999999999999999999999999999", &endptr, 10);
        ASSERT_LONG_EQ(val, LONG_MIN, "negative overflow clamps to LONG_MIN");
    }
    PASS();
}

static void test_strtol_negative(void)
{
    char *endptr;

    TEST("strtol negative value");
    {
        long val = strtol("-42", &endptr, 10);
        ASSERT_LONG_EQ(val, -42, "negative decimal");
        ASSERT(*endptr == '\0', "endptr at end");
    }
    PASS();

    TEST("strtol negative hex");
    {
        long val = strtol("-FF", &endptr, 16);
        ASSERT_LONG_EQ(val, -255, "negative hex");
    }
    PASS();

    TEST("strtol negative with sign and leading whitespace");
    {
        long val = strtol("  -42", &endptr, 10);
        ASSERT_LONG_EQ(val, -42, "whitespace then negative");
    }
    PASS();
}

static void test_strtol_leading_whitespace(void)
{
    char *endptr;

    TEST("strtol leading whitespace consumed");
    {
        long val = strtol("   \t  123", &endptr, 10);
        ASSERT_LONG_EQ(val, 123, "leading space/tab consumed");
    }
    PASS();

    TEST("strtol leading mixed whitespace with positive sign");
    {
        long val = strtol("  \n  +456", &endptr, 10);
        ASSERT_LONG_EQ(val, 456, "leading newline + plus consumed");
    }
    PASS();

    TEST("strtol leading whitespace with negative and large number");
    {
        long val = strtol("  \t  -99999", &endptr, 10);
        ASSERT_LONG_EQ(val, -99999, "leading tab + negative large");
    }
    PASS();
}

/* ===================================================================
 *  CATEGORY 2: Memory operation edge cases
 *
 *  Tests: memset zero-length, memcpy overlapping (if handled),
 *         memcmp identical of various sizes, memcmp differing first byte
 * =================================================================== */

static void test_memset_zero_length(void)
{
    TEST("memset with zero length does nothing");
    {
        uint8_t buf[16];
        memset(buf, 0xAA, sizeof(buf));
        memset(buf, 0x00, 0);
        ASSERT_INT_EQ(buf[0], 0xAA, "first byte unchanged");
        ASSERT_INT_EQ(buf[15], 0xAA, "last byte unchanged");
    }
    PASS();
}

static void test_memcpy_edge(void)
{
    TEST("memcpy size=0 (no-op)");
    {
        char src[8] = "source!";
        char dst[8] = "target!";
        memcpy(dst, src, 0);
        ASSERT_STR_EQ(dst, "target!", "no bytes copied");
    }
    PASS();

    TEST("memcpy exact size copy then verify");
    {
        char buf[16];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, "Hello, World!", 14);
        ASSERT_INT_EQ(memcmp(buf, "Hello, World!", 14), 0, "content matches");
    }
    PASS();
}

static void test_memcpy_large_copy(void)
{
    TEST("memcpy large buffer (1 MB pattern)");
    {
        size_t sz = 1024 * 1024;
        uint8_t *src = (uint8_t *)malloc(sz);
        uint8_t *dst = (uint8_t *)malloc(sz);
        ASSERT(src != NULL && dst != NULL, "malloc success");
        for (size_t i = 0; i < sz; i++)
            src[i] = (uint8_t)(i & 0xFF);
        memset(dst, 0, sz);
        memcpy(dst, src, sz);
        int ok = memcmp(src, dst, sz) == 0;
        ASSERT(ok, "large memcpy integrity");
        free(src);
        free(dst);
    }
    PASS();
}

static void test_memcmp_identical_various(void)
{
    TEST("memcmp identical buffers of size 0");
    {
        ASSERT_INT_EQ(memcmp("", "", 0), 0, "zero-length identical");
    }
    PASS();

    TEST("memcmp identical buffers of size 1");
    {
        uint8_t a = 0x42, b = 0x42;
        ASSERT_INT_EQ(memcmp(&a, &b, 1), 0, "single byte identical");
    }
    PASS();

    TEST("memcmp identical buffers of size 7 (odd)");
    {
        uint8_t a[7] = {1,2,3,4,5,6,7};
        uint8_t b[7] = {1,2,3,4,5,6,7};
        ASSERT_INT_EQ(memcmp(a, b, 7), 0, "7 bytes identical");
    }
    PASS();

    TEST("memcmp identical buffers of size 64 (power of 2)");
    {
        uint8_t a[64], b[64];
        memset(a, 0xAB, 64);
        memset(b, 0xAB, 64);
        ASSERT_INT_EQ(memcmp(a, b, 64), 0, "64 bytes identical");
    }
    PASS();

    TEST("memcmp identical buffers of size 4096");
    {
        size_t sz = 4096;
        uint8_t *a = (uint8_t *)malloc(sz);
        uint8_t *b = (uint8_t *)malloc(sz);
        ASSERT(a != NULL && b != NULL, "malloc");
        memset(a, 0x55, sz);
        memset(b, 0x55, sz);
        ASSERT_INT_EQ(memcmp(a, b, sz), 0, "4096 bytes identical");
        free(a);
        free(b);
    }
    PASS();
}

static void test_memcmp_diff_first_byte(void)
{
    TEST("memcmp differs on first byte");
    {
        uint8_t a[8] = {0xFF, 2, 3, 4, 5, 6, 7, 8};
        uint8_t b[8] = {0xFE, 2, 3, 4, 5, 6, 7, 8};
        ASSERT(memcmp(a, b, 8) != 0, "differs on byte 0");
    }
    PASS();

    TEST("memcmp differs on last byte");
    {
        uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 0xFF};
        uint8_t b[8] = {1, 2, 3, 4, 5, 6, 7, 0xFE};
        ASSERT(memcmp(a, b, 8) != 0, "differs on byte 7");
    }
    PASS();

    TEST("memcmp differs at exact middle byte");
    {
        uint8_t a[16] = {0};
        uint8_t b[16] = {0};
        a[7] = 0x01;
        b[7] = 0x02;
        ASSERT(memcmp(a, b, 16) != 0, "differs at index 7");
    }
    PASS();

    TEST("memcmp zero-length returns 0 even for different ptrs");
    {
        int r = memcmp("abc", "xyz", 0);
        ASSERT_INT_EQ(r, 0, "zero length returns 0");
    }
    PASS();
}

/* ===================================================================
 *  CATEGORY 3: Bitfield operation tests
 *
 *  Tests BIT(), GENMASK(), FIELD_GET(), FIELD_PREP() macros
 *  including boundary conditions.
 * =================================================================== */

static void test_bitfield_bit_zero(void)
{
    TEST("BIT(0) == 1");
    ASSERT_INT_EQ(BIT(0), 1ULL, "BIT(0) = 1");
    PASS();

    TEST("BIT(0) sets bit 0 in register");
    {
        volatile uint64_t reg = 0;
        reg |= BIT(0);
        ASSERT_INT_EQ((int)(reg & 1), 1, "reg bit 0 set");
    }
    PASS();
}

static void test_bitfield_write_register(void)
{
    TEST("Write entire register via FIELD_PREP with GENMASK(63,0)");
    {
        uint64_t val = 0xDEADBEEFCAFE1234ULL;
        uint64_t reg = FIELD_PREP(GENMASK(63, 0), val);
        ASSERT(reg == val, "full register write matches value");
    }
    PASS();

    TEST("FIELD_PREP writes to all bits and FIELD_GET reads back");
    {
        uint64_t reg = FIELD_PREP(GENMASK(63, 0), 0xABCD1234DEADBEEFULL);
        uint64_t readback = FIELD_GET(GENMASK(63, 0), reg);
        ASSERT(readback == 0xABCD1234DEADBEEFULL,
               "readback matches written value");
    }
    PASS();
}

static void test_bitfield_cross_byte(void)
{
    TEST("Reading/writing field across byte boundary (bits 11:4)");
    {
        uint64_t value = 0xAB;
        uint64_t reg = FIELD_PREP(GENMASK(11, 4), value);
        uint64_t readback = FIELD_GET(GENMASK(11, 4), reg);
        ASSERT(readback == value,
               "cross-byte field roundtrip (11:4)");
    }
    PASS();

    TEST("Reading/writing field across 4-byte boundary (bits 31:16)");
    {
        uint64_t value = 0xABCD;
        uint64_t reg = FIELD_PREP(GENMASK(31, 16), value);
        uint64_t readback = FIELD_GET(GENMASK(31, 16), reg);
        ASSERT(readback == value,
               "cross-4byte field roundtrip (31:16)");
    }
    PASS();

    TEST("Reading/writing field across 8-byte boundary (bits 47:40)");
    {
        uint64_t value = 0x42;
        uint64_t reg = FIELD_PREP(GENMASK(47, 40), value);
        uint64_t readback = FIELD_GET(GENMASK(47, 40), reg);
        ASSERT(readback == value,
               "cross-48bit boundary roundtrip (47:40)");
    }
    PASS();
}

static void test_bitfield_boundary(void)
{
    TEST("BIT(63) == 1ULL << 63 (MSB)");
    ASSERT(BIT(63) == (1ULL << 63), "BIT(63) is MSB");
    PASS();

    TEST("FIELD_GET at bit 63 returns correct value");
    {
        uint64_t val = FIELD_GET(GENMASK(63, 63), BIT(63));
        ASSERT(val == 1, "bit 63 extracted as 1");
    }
    PASS();

    TEST("FIELD_PREP at bit 63 sets MSB");
    {
        uint64_t reg = FIELD_PREP(GENMASK(63, 63), 1);
        ASSERT(reg == BIT(63), "MSB set via FIELD_PREP");
    }
    PASS();

    TEST("GENMASK(63,0) covers all 64 bits");
    {
        ASSERT(GENMASK(63, 0) == ~0ULL,
               "GENMASK(63,0) = ~0ULL");
    }
    PASS();

    TEST("FIELD_GET/GENMASK roundtrip at bit 62");
    {
        uint64_t reg = FIELD_PREP(GENMASK(62, 62), 1);
        uint64_t rb = FIELD_GET(GENMASK(62, 62), reg);
        ASSERT(rb == 1, "bit 62 roundtrip");
    }
    PASS();
}

static void test_bitfield_multi_field(void)
{
    TEST("Two non-overlapping fields in one register");
    {
        uint64_t reg = FIELD_PREP(GENMASK(7, 0), 0xAB)
                     | FIELD_PREP(GENMASK(15, 8), 0xCD);
        ASSERT(FIELD_GET(GENMASK(7, 0), reg) == 0xAB &&
               FIELD_GET(GENMASK(15, 8), reg) == 0xCD,
               "two nibbles in one register");
    }
    PASS();

    TEST("Three non-overlapping fields (8-bit, 8-bit, 16-bit)");
    {
        uint64_t reg = FIELD_PREP(GENMASK(7, 0), 0x12)
                     | FIELD_PREP(GENMASK(15, 8), 0x34)
                     | FIELD_PREP(GENMASK(31, 16), 0x5678);
        ASSERT(FIELD_GET(GENMASK(7, 0), reg) == 0x12 &&
               FIELD_GET(GENMASK(15, 8), reg) == 0x34 &&
               FIELD_GET(GENMASK(31, 16), reg) == 0x5678,
               "three fields combined");
    }
    PASS();

    TEST("Four non-overlapping 8-bit fields in 32-bit register");
    {
        uint64_t reg = FIELD_PREP(GENMASK(7, 0), 0x01)
                     | FIELD_PREP(GENMASK(15, 8), 0x23)
                     | FIELD_PREP(GENMASK(23, 16), 0x45)
                     | FIELD_PREP(GENMASK(31, 24), 0x67);
        ASSERT(FIELD_GET(GENMASK(7, 0), reg) == 0x01 &&
               FIELD_GET(GENMASK(15, 8), reg) == 0x23 &&
               FIELD_GET(GENMASK(23, 16), reg) == 0x45 &&
               FIELD_GET(GENMASK(31, 24), reg) == 0x67,
               "four 8-bit fields");
    }
    PASS();
}

/* ===================================================================
 *  CATEGORY 4: Negative-path / error-handling tests
 *
 *  Functions with NULL pointers, zero-size arguments,
 *  edge boundary values.
 * =================================================================== */

static void test_negative_null_ptrs(void)
{
    TEST("strchr with empty string returns NULL");
    {
        char *p = strchr("", 'x');
        ASSERT(p == NULL, "strchr on empty returns NULL");
    }
    PASS();

    TEST("strstr with empty haystack returns NULL");
    {
        char *p = strstr("", "x");
        ASSERT(p == NULL, "strstr('', 'x') returns NULL");
    }
    PASS();

    TEST("strstr with empty needle returns haystack");
    {
        char s[] = "hello";
        char *p = strstr(s, "");
        ASSERT(p == s, "strstr(s, '') returns s");
    }
    PASS();

    TEST("strrchr on single-char string finds the char");
    {
        char *p = strrchr("a", 'a');
        ASSERT(p != NULL && *p == 'a', "strrchr('a','a') finds it");
    }
    PASS();
}

static void test_negative_zero_size(void)
{
    TEST("memcmp with n=0 returns 0 (even if ptrs differ)");
    {
        int r = memcmp("abc", "xyz", 0);
        ASSERT_INT_EQ(r, 0, "zero-length memcmp returns 0");
    }
    PASS();

    TEST("strncmp with n=0 returns 0");
    {
        int r = strncmp("anything", "different", 0);
        ASSERT_INT_EQ(r, 0, "zero-length strncmp returns 0");
    }
    PASS();

    TEST("strncat with zero length appends nothing");
    {
        char buf[16] = "hello";
        strncat(buf, " world", 0);
        ASSERT_STR_EQ(buf, "hello", "strncat with n=0 does nothing");
    }
    PASS();

    TEST("strncpy with n=0 copies nothing");
    {
        char buf[16] = "original";
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
        strncpy(buf, "new", 0);
#pragma GCC diagnostic pop
        ASSERT_STR_EQ(buf, "original", "strncpy with n=0 does nothing");
    }
    PASS();
}

static void test_negative_boundary(void)
{
    char *endptr;

    TEST("strtol with base 0 handles leading 0 as octal");
    {
        long val = strtol("077", &endptr, 0);
        ASSERT_LONG_EQ(val, 63, "077 -> octal 63");
    }
    PASS();

    TEST("strtol with base 1 returns 0 (invalid base)");
    {
        long val = strtol("123", &endptr, 1);
        ASSERT_LONG_EQ(val, 0, "base 1 is invalid -> 0");
    }
    PASS();

    TEST("strtol with base 36 (max valid)");
    {
        long val = strtol("zz", &endptr, 36);
        ASSERT_LONG_EQ(val, 35*36 + 35, "base 36 'zz' -> 1295");
    }
    PASS();

    TEST("strtol with base 37 returns 0 (invalid)");
    {
        long val = strtol("123", &endptr, 37);
        ASSERT_LONG_EQ(val, 0, "base 37 invalid -> 0");
    }
    PASS();

    TEST("strtol with INT_MAX boundary");
    {
        long val = strtol("2147483647", &endptr, 10);
        ASSERT_LONG_EQ(val, 2147483647L, "INT_MAX");
    }
    PASS();

    TEST("strtol with INT_MIN boundary");
    {
        long val = strtol("-2147483648", &endptr, 10);
        ASSERT_LONG_EQ(val, -2147483648L, "INT_MIN");
    }
    PASS();
}

static void test_negative_empty_inputs(void)
{
    char *endptr;

    TEST("strtol empty string returns 0");
    {
        long val = strtol("", &endptr, 10);
        ASSERT_LONG_EQ(val, 0, "empty string -> 0");
        ASSERT(endptr == (void *)"", "endptr at start");
    }
    PASS();

    TEST("strtol just '+' returns 0");
    {
        long val = strtol("+", &endptr, 10);
        ASSERT_LONG_EQ(val, 0, "just '+' -> 0");
    }
    PASS();

    TEST("strtol just '-' returns 0");
    {
        long val = strtol("-", &endptr, 10);
        ASSERT_LONG_EQ(val, 0, "just '-' -> 0");
    }
    PASS();

    TEST("strtrim of single space returns empty");
    {
        char s[] = " ";
        ASSERT_STR_EQ(strtrim_local(s), "", "single space trimmed");
    }
    PASS();

    TEST("strtrim of tabs-only returns empty");
    {
        char s[] = "\t\t";
        ASSERT_STR_EQ(strtrim_local(s), "", "tabs trimmed");
    }
    PASS();

    TEST("memcmp self-comparison of large zero-filled block");
    {
        size_t sz = 65536;
        uint8_t *buf = (uint8_t *)malloc(sz);
        ASSERT(buf != NULL, "malloc");
        memset(buf, 0, sz);
        ASSERT_INT_EQ(memcmp(buf, buf, sz), 0, "self-compare all zeros");
        free(buf);
    }
    PASS();
}

static void test_negative_memmove_edge(void)
{
    TEST("memmove with src == dst (no-op)");
    {
        char buf[] = "hello";
        memmove(buf, buf, 5);
        ASSERT_STR_EQ(buf, "hello", "self-move unchanged");
    }
    PASS();

    TEST("memmove with size 0 (no-op)");
    {
        char buf[] = "hello";
        memmove(buf, buf + 1, 0);
        ASSERT_STR_EQ(buf, "hello", "zero-size memmove unchanged");
    }
    PASS();

    TEST("memmove overlapping forward (src < dst)");
    {
        char buf[] = "abcdefghij";
        memmove(buf + 3, buf, 5);
        ASSERT_INT_EQ(memcmp(buf, "abcabcdeij", 10), 0, "forward overlap");
    }
    PASS();

    TEST("memmove overlapping backward (src > dst)");
    {
        char buf[] = "abcdefghij";
        memmove(buf, buf + 3, 5);
        ASSERT_INT_EQ(memcmp(buf, "defghfghij", 10), 0, "backward overlap");
    }
    PASS();
}

/* ===================================================================
 *  Main: run all test groups
 * =================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  Negative-Path / Edge-Case Test Suite\n");
    printf("============================================\n\n");

    /* ── Category 1: String edge cases (11 tests) ──── */
    printf("--- String Edge Cases ---\n");
    test_strlcpy_zero_dst();
    test_strlcat_full_buffer();
    test_strtrim_all_whitespace();
    test_strtrim_empty();
    test_strtol_overflow();
    test_strtol_negative();
    test_strtol_leading_whitespace();

    /* ── Category 2: Memory operation edge cases (13 tests) ──── */
    printf("\n--- Memory Operation Edge Cases ---\n");
    test_memset_zero_length();
    test_memcpy_edge();
    test_memcpy_large_copy();
    test_memcmp_identical_various();
    test_memcmp_diff_first_byte();

    /* ── Category 3: Bitfield operations (14 tests) ──── */
    printf("\n--- Bitfield Operations ---\n");
    test_bitfield_bit_zero();
    test_bitfield_write_register();
    test_bitfield_cross_byte();
    test_bitfield_boundary();
    test_bitfield_multi_field();

    /* ── Category 4: Negative-path / error-handling (17 tests) ──── */
    printf("\n--- Negative-Path / Error-Handling ---\n");
    test_negative_null_ptrs();
    test_negative_zero_size();
    test_negative_boundary();
    test_negative_empty_inputs();
    test_negative_memmove_edge();

    /* ── Summary ──── */
    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
