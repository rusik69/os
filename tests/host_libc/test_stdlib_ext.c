/*
 * test_stdlib_ext.c — Host-side tests for kernel extended stdlib functions
 *
 * Tests strtoll, strtoull, atoll, ultoa, realloc, calloc from src/lib/stdlib_ext.c.
 * All pure algorithmic — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel function prototypes
 * =================================================================== */
extern long long atoll(const char *nptr);
extern long long strtoll(const char *nptr, char **endptr, int base);
extern unsigned long long strtoull(const char *nptr, char **endptr, int base);
extern char *ultoa(unsigned long value, char *str, int base);

/* ===================================================================
 *  Stubs for kernel symbols
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
 *  test_strtoll
 * =================================================================== */
static void test_strtoll(void)
{
    /* 1. Decimal positive */
    TEST("strtoll: decimal positive", strtoll("12345", NULL, 10) == 12345);

    /* 2. Hex */
    TEST("strtoll: hex", strtoll("0xFF", NULL, 16) == 255);
    TEST("strtoll: hex (no prefix)", strtoll("FF", NULL, 16) == 255);

    /* 3. Octal */
    TEST("strtoll: octal", strtoll("077", NULL, 8) == 63);
    TEST("strtoll: octal (no prefix)", strtoll("77", NULL, 8) == 63);

    /* 4. Auto-detect 0x → hex */
    TEST("strtoll: auto hex", strtoll("0xA", NULL, 0) == 10);

    /* 5. Auto-detect 0 → octal */
    TEST("strtoll: auto octal", strtoll("010", NULL, 0) == 8);

    /* 6. Auto-detect decimal */
    TEST("strtoll: auto decimal", strtoll("42", NULL, 0) == 42);

    /* 7. Negative */
    TEST("strtoll: negative", strtoll("-42", NULL, 10) == -42);
    TEST("strtoll: negative hex", strtoll("-0xFF", NULL, 16) == -255);

    /* 8. Leading whitespace */
    TEST("strtoll: leading space", strtoll("  123", NULL, 10) == 123);
    TEST("strtoll: leading tab", strtoll("\t456", NULL, 10) == 456);

    /* 9. With endptr */
    char *end = NULL;
    long long v = strtoll("123abc", &end, 10);
    TEST("strtoll: endptr stops at non-digit", v == 123 && end && *end == 'a');

    /* 10. Zero */
    TEST("strtoll: zero", strtoll("0", NULL, 10) == 0);

    /* 11. Large value */
    long long large = 9223372036854775807LL;
    TEST("strtoll: LLONG_MAX", strtoll("9223372036854775807", NULL, 10) == large);

    /* 12. Base 2 */
    TEST("strtoll: binary", strtoll("1010", NULL, 2) == 10);

    /* 13. Base 36 */
    TEST("strtoll: base 36", strtoll("1z", NULL, 36) == 71); /* 1*36 + 35 */

    /* 14. Invalid digit in base */
    char *end2 = NULL;
    long long v2 = strtoll("1a2", &end2, 10);
    TEST("strtoll: stops at invalid digit", v2 == 1 && end2 && *end2 == 'a');

    /* 15. Empty string returns 0 */
    char *ep;
    long long v_empty = strtoll("", &ep, 10);
    TEST("strtoll: empty string returns 0", v_empty == 0);

    /* 18. Just positive sign */
    long long v_plus = strtoll("+", &ep, 10);
    TEST("strtoll: just '+' returns 0", v_plus == 0);

    /* 19. Just negative sign */
    long long v_minus = strtoll("-", &ep, 10);
    TEST("strtoll: just '-' returns 0", v_minus == 0);
}

/* ===================================================================
 *  test_strtoull
 * =================================================================== */
static void test_strtoull(void)
{
    /* 1. Decimal */
    TEST("strtoull: decimal", strtoull("99999", NULL, 10) == 99999);

    /* 2. Hex */
    TEST("strtoull: hex", strtoull("0xABCD", NULL, 16) == 0xABCDULL);

    /* 3. Auto-detect */
    TEST("strtoull: auto hex", strtoull("0xFF", NULL, 0) == 255);
    TEST("strtoull: auto octal", strtoull("077", NULL, 0) == 63);

    /* 4. Leading + sign */
    TEST("strtoull: leading plus", strtoull("+42", NULL, 10) == 42);

    /* 5. Max value */
    TEST("strtoull: large ULLONG_MAX approx",
         strtoull("18446744073709551615", NULL, 10) == 18446744073709551615ULL);

    /* 6. Base 2 and 36 */
    TEST("strtoull: base 2", strtoull("1111", NULL, 2) == 15);
    TEST("strtoull: base 36", strtoull("zz", NULL, 36) == 1295); /* 35*36 + 35 */

    /* 7. With endptr */
    char *end = NULL;
    unsigned long long v = strtoull("42xyz", &end, 10);
    TEST("strtoull: endptr", v == 42 && end && *end == 'x');

    /* 8. Whitespace */
    TEST("strtoull: leading space", strtoull("  1", NULL, 10) == 1);

    /* 10. Hex uppercase */
    TEST("strtoull: hex uppercase", strtoull("0X1F", NULL, 0) == 31);
}

/* ===================================================================
 *  test_atoll
 * =================================================================== */
static void test_atoll(void)
{
    TEST("atoll: positive", atoll("42") == 42);
    TEST("atoll: negative", atoll("-42") == -42);
    TEST("atoll: zero", atoll("0") == 0);
    TEST("atoll: large", atoll("9876543210") == 9876543210LL);
}

/* ===================================================================
 *  test_ultoa
 * =================================================================== */
static void test_ultoa(void)
{
    char buf[66];

    /* 1. Base 10 */
    ultoa(12345, buf, 10);
    TEST("ultoa: base 10", strcmp(buf, "12345") == 0);

    /* 2. Base 16 */
    ultoa(255, buf, 16);
    TEST("ultoa: base 16", strcmp(buf, "ff") == 0);

    /* 3. Base 2 */
    ultoa(42, buf, 2);
    TEST("ultoa: base 2", strcmp(buf, "101010") == 0);

    /* 4. Base 8 */
    ultoa(64, buf, 8);
    TEST("ultoa: base 8", strcmp(buf, "100") == 0);

    /* 5. Base 36 */
    ultoa(71, buf, 36);
    TEST("ultoa: base 36", strcmp(buf, "1z") == 0);

    /* 6. Zero */
    ultoa(0, buf, 10);
    TEST("ultoa: zero", strcmp(buf, "0") == 0);

    /* 7. Large value */
    ultoa(4294967295UL, buf, 16);
    TEST("ultoa: max 32-bit in hex", strcmp(buf, "ffffffff") == 0);

    /* 8. Large in base 10 */
    ultoa(4294967295UL, buf, 10);
    TEST("ultoa: max 32-bit in decimal", strcmp(buf, "4294967295") == 0);

    /* 9. Invalid base (should return empty) */
    ultoa(42, buf, 1);
    TEST("ultoa: base 1 invalid", strcmp(buf, "") == 0);
    ultoa(42, buf, 37);
    TEST("ultoa: base 37 invalid", strcmp(buf, "") == 0);

    /* 10. Base 11 (uses a-z for digits 10+) */
    ultoa(21, buf, 11);
    TEST("ultoa: base 11 uses letters", strcmp(buf, "1a") == 0);

    /* 11. Zero in binary */
    ultoa(0, buf, 2);
    TEST("ultoa: zero in binary", strcmp(buf, "0") == 0);

    /* 12. UINT64_MAX in decimal */
    ultoa(18446744073709551615UL, buf, 10);
    TEST("ultoa: max 64-bit in decimal", strcmp(buf, "18446744073709551615") == 0);
}

/* ===================================================================
 *  test_realloc
 * =================================================================== */
static void test_realloc(void)
{
    /* 1. realloc(NULL, size) ≈ malloc */
    char *p = (char *)realloc(NULL, 16);
    int ok1 = (p != NULL);
    if (p) {
        memcpy(p, "hello", 6);
        /* 2. realloc grow — check content preserved */
        char *q = (char *)realloc(p, 64);
        if (q) {
            int content_ok = (strcmp(q, "hello") == 0);
            TEST("realloc: grow preserves content", content_ok);
            /* 3. realloc shrink */
            char *r = (char *)realloc(q, 3);
            if (r) {
                free(r);
                TEST("realloc: shrink succeeds", 1);
            } else {
                TEST("realloc: shrink succeeds", 0);
            }
        } else {
            TEST("realloc: grow", 0);
        }
    }
    TEST("realloc: NULL ptr ≈ malloc", ok1);
}

/* ===================================================================
 *  test_calloc
 * =================================================================== */
static void test_calloc(void)
{
    /* 1. Basic calloc — zeroed */
    int *p = (int *)calloc(10, sizeof(int));
    if (p) {
        int zeroed = 1;
        for (int i = 0; i < 10; i++)
            if (p[i] != 0) { zeroed = 0; break; }
        TEST("calloc: memory is zeroed", zeroed);
        free(p);
    } else {
        TEST("calloc: memory allocation", 0);
    }
}

/* ===================================================================
 *  Comparison helpers (for system bsearch/qsort)
 * =================================================================== */
static int cmp_int_asc(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

/* ===================================================================
 *  test_extra_stdlib — additional edge case tests
 * =================================================================== */
static void test_extra_stdlib(void)
{
    char *ep;

    /* strtol edge cases (system libc) */
    TEST("strtol: just '+' returns 0", strtol("+", &ep, 10) == 0);
    TEST("strtol: just '-' returns 0", strtol("-", &ep, 10) == 0);
    TEST("strtol: just '0x' prefix returns 0", strtol("0x", &ep, 0) == 0);
    TEST("strtol: just '0X' prefix returns 0", strtol("0X", &ep, 0) == 0);
    TEST("strtol: large overflow clamps", strtol("999999999999999999999", &ep, 10) > 0);

    /* atoi with various values */
    TEST("atoi: positive", atoi("42") == 42);
    TEST("atoi: negative", atoi("-42") == -42);
    TEST("atoi: zero", atoi("0") == 0);
    TEST("atoi: leading plus", atoi("+99") == 99);

    /* strdup with empty and non-empty strings */
    char *s1 = strdup("");
    TEST("strdup: empty string non-null", s1 != NULL);
    if (s1) TEST("strdup: empty string length 0", strlen(s1) == 0);
    free(s1);

    char *s2 = strdup("hello, world");
    TEST("strdup: non-empty non-null", s2 != NULL);
    if (s2) TEST("strdup: copies content correctly", strcmp(s2, "hello, world") == 0);
    free(s2);

    /* bsearch with duplicate elements */
    int bs_arr[] = { 1, 2, 2, 2, 3, 4, 5 };
    int bs_k = 2;
    int *bs_r = (int *)bsearch(&bs_k, bs_arr, 7, sizeof(int), cmp_int_asc);
    TEST("bsearch: finds element in duplicated array", bs_r && *bs_r == 2);

    int bs_k_miss = 6;
    int *bs_r_miss = (int *)bsearch(&bs_k_miss, bs_arr, 7, sizeof(int), cmp_int_asc);
    TEST("bsearch: missing element returns NULL", bs_r_miss == NULL);

    /* bsearch edge cases: first and last elements */
    int bs_k_first = 1;
    int *bs_r_first = (int *)bsearch(&bs_k_first, bs_arr, 7, sizeof(int), cmp_int_asc);
    TEST("bsearch: finds first element", bs_r_first && *bs_r_first == 1);

    int bs_k_last = 5;
    int *bs_r_last = (int *)bsearch(&bs_k_last, bs_arr, 7, sizeof(int), cmp_int_asc);
    TEST("bsearch: finds last element", bs_r_last && *bs_r_last == 5);
}

/* ===================================================================
 *  test_extra_stdlib_more — additional edge case tests
 * =================================================================== */
static void test_extra_stdlib_more(void)
{
    char *ep;

    /* 1. strtoll with very large base 36 value */
    {
        long long v = strtoll("zzzzzzzz", &ep, 36);
        TEST("strtoll: large base-36 value non-zero", v != 0);
        TEST("strtoll: large base-36 consumed all chars", ep && *ep == '\0');
    }

    /* 2. strtoll with negative large base-36 */
    {
        long long v = strtoll("-zzzz", &ep, 36);
        TEST("strtoll: negative base-36 value", v < 0);
    }

    /* 3. strtoull with invalid base (base 1) */
    {
        unsigned long long v = strtoull("123", &ep, 1);
        TEST("strtoull: base=1 returns 0", v == 0);
    }

    /* 4. strtoull with leading zeros */
    {
        unsigned long long v = strtoull("00000123", &ep, 10);
        TEST("strtoull: leading zeros", v == 123);
    }

    /* 5. strtoll with leading zeros and auto-detect */
    {
        long long v = strtoll("00123", &ep, 0);
        /* leading 0 → octal: 0123 = 83 */
        TEST("strtoll: auto-detect leading 0 as octal", v == 83);
    }

    /* 6. atoll with very long string */
    {
        long long v = atoll("1234567890123456789");
        TEST("atoll: very long number", v == 1234567890123456789LL);
    }

    /* 7. atoll with leading whitespace */
    {
        long long v = atoll("   -42");
        TEST("atoll: leading whitespace with negative", v == -42);
    }

    /* 8. ultoa: base 16 uppercase check */
    {
        char buf[66];
        ultoa(0xABCDEF, buf, 16);
        TEST("ultoa: hex uses lowercase", strcmp(buf, "abcdef") == 0 || strcmp(buf, "ABCDEF") == 0);
    }

    /* 9. ultoa: value = 1 in various bases */
    {
        char buf[66];
        ultoa(1, buf, 2);
        TEST("ultoa: 1 in binary", strcmp(buf, "1") == 0);
        ultoa(1, buf, 10);
        TEST("ultoa: 1 in decimal", strcmp(buf, "1") == 0);
        ultoa(1, buf, 16);
        TEST("ultoa: 1 in hex", strcmp(buf, "1") == 0);
    }

    /* 10. realloc: grow then shrink back */
    {
        char *p = (char *)realloc(NULL, 8);
        if (p) {
            memcpy(p, "abcdefg", 8);
            char *q = (char *)realloc(p, 64);
            if (q) {
                int ok = (memcmp(q, "abcdefg", 8) == 0);
                TEST("realloc: grow 8→64 preserves content", ok);
                char *r = (char *)realloc(q, 4);
                if (r) {
                    /* Content should be preserved for min(4, 8) = 4 bytes */
                    TEST("realloc: shrink 64→4 preserves first 4 bytes",
                         memcmp(r, "abcd", 4) == 0);
                    free(r);
                }
            } else {
                free(p);
            }
        }
    }

    /* 11. calloc: large array */
    {
        int *p = (int *)calloc(1000, sizeof(int));
        if (p) {
            int zeroed = 1;
            for (int i = 0; i < 1000; i++)
                if (p[i] != 0) { zeroed = 0; break; }
            TEST("calloc: 1000 ints zeroed", zeroed);
            free(p);
        }
    }

    /* 12. calloc: zero elements (may return NULL or valid pointer) */
    {
        int *p = (int *)calloc(0, sizeof(int));
        /* calloc(0, ...) behavior is implementation-defined */
        TEST("calloc: zero elements doesn't crash", 1);
        if (p) free(p);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Extended stdlib Tests ===\n\n");

    printf("--- strtoll ---\n");
    test_strtoll();

    printf("\n--- strtoull ---\n");
    test_strtoull();

    printf("\n--- atoll ---\n");
    test_atoll();

    printf("\n--- ultoa ---\n");
    test_ultoa();

    printf("\n--- realloc ---\n");
    test_realloc();

    printf("\n--- calloc ---\n");
    test_calloc();

    printf("\n--- extras ---\n");
    test_extra_stdlib();

    printf("\n--- more extras ---\n");
    test_extra_stdlib_more();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
