/*
 * test_string.c — Host-side unit tests for kernel string operations
 *
 * Tests strlen, strcmp, strncmp, memcpy, memset, memcmp, strncpy,
 * strncat, strchr, strrchr, strstr, memmove, strcpy, strcat, and
 * snprintf — all the ISO C string functions used in kernel space.
 *
 * Runs entirely on the host — no kernel dependencies.
 *
 * Compile:  gcc -Wall -Werror -g -O0 -o test_string test_string.c
 * Run:      ./test_string
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define ASSERT_PTR_NONNULL(ptr, msg) do {       \
    if ((ptr) == NULL) {                        \
        tests_failed++;                         \
        printf("FAIL\n");                       \
        printf("        %s\n", msg);            \
        printf("        Expected non-NULL\n");  \
        return;                                 \
    }                                           \
} while (0)

/* ===================================================================
 *  Tests: strlen
 * =================================================================== */

static void test_strlen_basic(void)
{
    TEST("strlen of empty string");
    ASSERT_INT_EQ(strlen(""), 0, "empty string length 0");
    PASS();

    TEST("strlen of short string");
    ASSERT_INT_EQ(strlen("hello"), 5, "hello length 5");
    PASS();

    TEST("strlen of longer string");
    ASSERT_INT_EQ(strlen("Hello, world!"), 13, "Hello, world! length 13");
    PASS();

    TEST("strlen of single char");
    ASSERT_INT_EQ(strlen("X"), 1, "single char length 1");
    PASS();
}

static void test_strlen_edge(void)
{
    TEST("strlen of string with null in middle (stops at first null)");
    /* Note: strlen stops at the first null, so we're really testing
     * that it doesn't go past. */
    ASSERT_INT_EQ(strlen("ab\0cd"), 2, "stops at embedded null");
    PASS();
}

/* ===================================================================
 *  Tests: strcmp / strncmp
 * =================================================================== */

static void test_strcmp_equal(void)
{
    TEST("strcmp of identical strings returns 0");
    ASSERT_INT_EQ(strcmp("hello", "hello"), 0, "identical");
    PASS();

    TEST("strcmp of empty strings returns 0");
    ASSERT_INT_EQ(strcmp("", ""), 0, "both empty");
    PASS();
}

static void test_strcmp_different(void)
{
    TEST("strcmp: first < second returns negative");
    ASSERT(strcmp("abc", "abd") < 0, "abc < abd");
    PASS();

    TEST("strcmp: first > second returns positive");
    ASSERT(strcmp("abd", "abc") > 0, "abd > abc");
    PASS();

    TEST("strcmp: shorter < longer prefix");
    ASSERT(strcmp("abc", "abcd") < 0, "abc < abcd");
    PASS();

    TEST("strcmp: longer > shorter prefix");
    ASSERT(strcmp("abcd", "abc") > 0, "abcd > abc");
    PASS();

    TEST("strcmp: case sensitivity");
    ASSERT(strcmp("Hello", "hello") != 0, "case-sensitive");
    PASS();
}

static void test_strncmp_equal(void)
{
    TEST("strncmp with n=0 returns 0");
    ASSERT_INT_EQ(strncmp("abc", "xyz", 0), 0, "zero length");
    PASS();

    TEST("strncmp identical up to n");
    ASSERT_INT_EQ(strncmp("abcde", "abcde", 3), 0, "first 3 match");
    PASS();

    TEST("strncmp identical fully");
    ASSERT_INT_EQ(strncmp("hello", "hello", 10), 0, "full match");
    PASS();
}

static void test_strncmp_different(void)
{
    TEST("strncmp different within n");
    ASSERT(strncmp("abcx", "abcy", 4) < 0, "abcx < abcy");
    PASS();

    TEST("strncmp different after n returns 0");
    ASSERT_INT_EQ(strncmp("abcX", "abcY", 3), 0, "different after n");
    PASS();
}

/* ===================================================================
 *  Tests: memcpy / memmove
 * =================================================================== */

static void test_memcpy_basic(void)
{
    char src[] = "Hello, world!";
    char dst[32];
    memset(dst, 0, sizeof(dst));

    TEST("memcpy copies exact bytes");
    memcpy(dst, src, 14);
    ASSERT_INT_EQ(memcmp(dst, src, 14), 0, "content match");
    PASS();
}

static void test_memcpy_zero(void)
{
    char src[] = "data";
    char dst[16] = {0};

    TEST("memcpy with zero length does nothing");
    memcpy(dst, src, 0);
    ASSERT_INT_EQ(dst[0], 0, "no bytes copied");
    PASS();
}

static void test_memcpy_overlap(void)
{
    /* memcpy doesn't handle overlapping regions — but we test it doesn't crash
     * for non-overlapping copies of various sizes */
    uint8_t buf[256];
    uint8_t ref[256];

    TEST("memcpy various sizes produces correct results");
    for (int size = 1; size <= 256; size++) {
        for (int i = 0; i < size; i++)
            buf[i] = (uint8_t)(i & 0xFF);
        memset(ref, 0, size);
        memcpy(ref, buf, (size_t)size);
        ASSERT_INT_EQ(memcmp(ref, buf, (size_t)size), 0,
                      "copy verification");
    }
    PASS();
}

static void test_memmove_overlap_forward(void)
{
    char buf[] = "abcdefghij";

    TEST("memmove overlapping forward (src < dst)");
    memmove(buf + 3, buf, 5);  /* move "abcde" to position 3 */
    ASSERT_INT_EQ(memcmp(buf, "abcabcdeij", 10), 0, "forward overlap");
    PASS();
}

static void test_memmove_overlap_backward(void)
{
    char buf[] = "abcdefghij";

    TEST("memmove overlapping backward (src > dst)");
    memmove(buf, buf + 3, 5);  /* move "defgh" to position 0 */
    ASSERT_INT_EQ(memcmp(buf, "defghfghij", 10), 0, "backward overlap");
    PASS();
}

static void test_memmove_no_overlap(void)
{
    char src[] = "Hello";
    char dst[16] = {0};

    TEST("memmove non-overlapping behaves like memcpy");
    memmove(dst, src, 6);
    ASSERT_INT_EQ(memcmp(dst, src, 6), 0, "identical to memcpy");
    PASS();
}

/* ===================================================================
 *  Tests: memset
 * =================================================================== */

static void test_memset_basic(void)
{
    uint8_t buf[32];

    TEST("memset fills with byte value");
    memset(buf, 0xAB, 32);
    for (int i = 0; i < 32; i++) {
        ASSERT_INT_EQ(buf[i], 0xAB, "byte fill");
    }
    PASS();
}

static void test_memset_zero(void)
{
    uint8_t buf[16];
    memset(buf, 0xFF, 16);

    TEST("memset with zero length does nothing");
    memset(buf, 0x00, 0);
    ASSERT_INT_EQ(buf[0], 0xFF, "no bytes changed");
    PASS();
}

static void test_memset_zero_fill(void)
{
    uint8_t buf[32];
    memset(buf, 0xFF, 32);

    TEST("memset to zero clears correctly");
    memset(buf, 0x00, 32);
    for (int i = 0; i < 32; i++) {
        ASSERT_INT_EQ(buf[i], 0x00, "zero fill");
    }
    PASS();
}

/* ===================================================================
 *  Tests: memcmp
 * =================================================================== */

static void test_memcmp_equal(void)
{
    uint8_t a[] = {1, 2, 3, 4, 5};
    uint8_t b[] = {1, 2, 3, 4, 5};

    TEST("memcmp equal buffers return 0");
    ASSERT_INT_EQ(memcmp(a, b, 5), 0, "equal");
    PASS();

    TEST("memcmp zero length returns 0");
    ASSERT_INT_EQ(memcmp(a, b, 0), 0, "zero length");
    PASS();
}

static void test_memcmp_different(void)
{
    uint8_t a[] = {1, 2, 3, 4, 5};
    uint8_t b[] = {1, 2, 3, 0xFF, 5};

    TEST("memcmp different buffers return non-zero");
    ASSERT(memcmp(a, b, 5) != 0, "different at position 3");
    PASS();

    TEST("memcmp stops at first difference");
    ASSERT_INT_EQ(memcmp(a, b, 3), 0, "first 3 match");
    PASS();
}

/* ===================================================================
 *  Tests: strncpy
 * =================================================================== */

static void test_strncpy_basic(void)
{
    char dst[32];
    memset(dst, 0xFF, sizeof(dst));

    TEST("strncpy copies and null-terminates");
    strncpy(dst, "hello", sizeof(dst));
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "content match");
    PASS();
}

static void test_strncpy_truncation(void)
{
    char dst[8];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"

    TEST("strncpy truncates long source");
    memset(dst, 0xAA, sizeof(dst));
    strncpy(dst, "Hello, world!", 8);
    /* strncpy copies up to n chars, doesn't null-terminate if n reached */
    ASSERT_INT_EQ(memcmp(dst, "Hello, w", 8), 0, "truncated copy");
#pragma GCC diagnostic pop
    PASS();
}

static void test_strncpy_exact_fit(void)
{
    char dst[6] = {0};

    TEST("strncpy with exact fit (source length = n-1)");
    strncpy(dst, "hello", 6);  /* "hello" + null */
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "exact fit");
    PASS();
}

/* ===================================================================
 *  Tests: strncat
 * =================================================================== */

static void test_strncat_basic(void)
{
    char dst[32] = "Hello";

    TEST("strncat appends string");
    strncat(dst, ", world!", 20);
    ASSERT_INT_EQ(strcmp(dst, "Hello, world!"), 0, "concatenation");
    PASS();
}

static void test_strncat_limit(void)
{
    char dst[16] = "ab";

    TEST("strncat respects length limit");
    strncat(dst, "cdefghijklmnop", 3);
    ASSERT_INT_EQ(strcmp(dst, "abcde"), 0, "limited concatenation (3 chars)");
    PASS();
}

static void test_strncat_empty_dst(void)
{
    char dst[32] = "";

    TEST("strncat to empty destination");
    strncat(dst, "hello", 10);
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "concat to empty");
    PASS();
}

/* ===================================================================
 *  Tests: strchr / strrchr
 * =================================================================== */

static void test_strchr_found(void)
{
    TEST("strchr finds character");
    char *p = strchr("hello world", 'w');
    ASSERT_PTR_NONNULL(p, "should find 'w'");
    ASSERT_INT_EQ(*p, 'w', "character match");
    PASS();

    TEST("strchr finds first occurrence");
    p = strchr("hello", 'l');
    ASSERT_PTR_NONNULL(p, "should find 'l'");
    ASSERT_INT_EQ(*p, 'l', "first l");
    /* Should point to the first 'l' at position 2 */
    ASSERT(p - "hello" == 2, "position 2");
    PASS();
}

static void test_strchr_not_found(void)
{
    TEST("strchr returns NULL when not found");
    char *p = strchr("hello", 'z');
    ASSERT(p == NULL, "should return NULL");
    PASS();
}

static void test_strchr_null_char(void)
{
    TEST("strchr of null terminator returns pointer to end");
    char s[] = "hello";
    char *p = strchr(s, '\0');
    ASSERT(p == s + 5, "pointer should be at end");
    PASS();
}

static void test_strrchr_found(void)
{
    TEST("strrchr finds last occurrence");
    char *p = strrchr("hello world", 'o');
    ASSERT_PTR_NONNULL(p, "should find 'o'");
    ASSERT_INT_EQ(*p, 'o', "character match");
    /* Should point to the 'o' in "world", not "hello" */
    ASSERT(p - "hello world" == 7, "position 7 (last o)");
    PASS();
}

static void test_strrchr_not_found(void)
{
    TEST("strrchr returns NULL when not found");
    char *p = strrchr("hello", 'z');
    ASSERT(p == NULL, "should return NULL");
    PASS();
}

/* ===================================================================
 *  Tests: strstr
 * =================================================================== */

static void test_strstr_found(void)
{
    TEST("strstr finds substring at start");
    char *p = strstr("hello world", "hello");
    ASSERT_PTR_NONNULL(p, "should find 'hello' at start");
    PASS();

    TEST("strstr finds substring in middle");
    p = strstr("hello world", "lo wo");
    ASSERT_PTR_NONNULL(p, "should find 'lo wo'");
    PASS();

    TEST("strstr finds substring at end");
    p = strstr("hello world", "world");
    ASSERT_PTR_NONNULL(p, "should find 'world' at end");
    PASS();

    TEST("strstr finds single character");
    p = strstr("hello", "e");
    ASSERT_PTR_NONNULL(p, "should find 'e'");
    PASS();
}

static void test_strstr_not_found(void)
{
    TEST("strstr returns NULL when substring not found");
    char *p = strstr("hello world", "xyz");
    ASSERT(p == NULL, "should return NULL");
    PASS();

    TEST("strstr handles empty haystack");
    p = strstr("", "x");
    ASSERT(p == NULL, "empty haystack returns NULL");
    PASS();
}

static void test_strstr_empty_needle(void)
{
    TEST("strstr with empty needle returns haystack");
    char s[] = "hello";
    char *p = strstr(s, "");
    ASSERT(p == s, "empty needle returns haystack");
    PASS();
}

/* ===================================================================
 *  Tests: strcpy / strcat (basic ISO C)
 * =================================================================== */

static void test_strcpy_basic(void)
{
    char dst[32];
    memset(dst, 0xFF, sizeof(dst));

    TEST("strcpy copies string including null terminator");
    strcpy(dst, "hello");
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "copied string");
    ASSERT_INT_EQ(dst[5], '\0', "null terminator present");
    PASS();
}

static void test_strcpy_empty(void)
{
    char dst[32] = {0};

    TEST("strcpy of empty string");
    strcpy(dst, "");
    ASSERT_INT_EQ(strlen(dst), 0, "empty copy");
    ASSERT_INT_EQ(dst[0], '\0', "null terminator");
    PASS();
}

static void test_strcat_basic(void)
{
    char dst[32] = "Hello";

    TEST("strcat appends to existing string");
    strcat(dst, ", world!");
    ASSERT_INT_EQ(strcmp(dst, "Hello, world!"), 0, "concatenation");
    PASS();
}

static void test_strcat_empty_dst(void)
{
    char dst[32] = "";

    TEST("strcat to empty destination");
    strcat(dst, "hello");
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "concat to empty");
    PASS();
}

static void test_strcat_empty_src(void)
{
    char dst[32] = "hello";

    TEST("strcat with empty source does nothing");
    strcat(dst, "");
    ASSERT_INT_EQ(strcmp(dst, "hello"), 0, "unchanged");
    PASS();
}

/* ===================================================================
 *  Tests: snprintf (basic integer/string formatting)
 * =================================================================== */

static void test_snprintf_basic(void)
{
    char buf[64];

    TEST("snprintf formats string correctly");
    int n = snprintf(buf, sizeof(buf), "hello");
    ASSERT(n >= 0, "snprintf should succeed");
    ASSERT_INT_EQ(strcmp(buf, "hello"), 0, "formatted string");
    PASS();
}

static void test_snprintf_int(void)
{
    char buf[64];

    TEST("snprintf formats integers");
    int n = snprintf(buf, sizeof(buf), "%d", 42);
    ASSERT(n > 0, "should have output");
    ASSERT_INT_EQ(strcmp(buf, "42"), 0, "integer formatting");
    PASS();
}

static void test_snprintf_multiple(void)
{
    char buf[64];

    TEST("snprintf formats multiple arguments");
    snprintf(buf, sizeof(buf), "%s %d", "value", 123);
    ASSERT_INT_EQ(strcmp(buf, "value 123"), 0, "multi-arg formatting");
    PASS();
}

static void test_snprintf_truncation(void)
{
    char buf[4];
    const char *long_str = "hello world";

    TEST("snprintf truncates output to buffer size");
    memset(buf, 0xAA, sizeof(buf));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    int n = snprintf(buf, sizeof(buf), "%s", long_str);
#pragma GCC diagnostic pop
    ASSERT(n > 0, "should return length needed");
    ASSERT_INT_EQ(buf[sizeof(buf) - 1], '\0', "should be null-terminated");
    PASS();
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("============================================\n");
    printf("  String Operation Unit Tests\n");
    printf("============================================\n\n");

    /* strlen */
    test_strlen_basic();
    test_strlen_edge();

    /* strcmp / strncmp */
    test_strcmp_equal();
    test_strcmp_different();
    test_strncmp_equal();
    test_strncmp_different();

    /* memcpy / memmove */
    test_memcpy_basic();
    test_memcpy_zero();
    test_memcpy_overlap();
    test_memmove_overlap_forward();
    test_memmove_overlap_backward();
    test_memmove_no_overlap();

    /* memset */
    test_memset_basic();
    test_memset_zero();
    test_memset_zero_fill();

    /* memcmp */
    test_memcmp_equal();
    test_memcmp_different();

    /* strncpy */
    test_strncpy_basic();
    test_strncpy_truncation();
    test_strncpy_exact_fit();

    /* strncat */
    test_strncat_basic();
    test_strncat_limit();
    test_strncat_empty_dst();

    /* strchr / strrchr */
    test_strchr_found();
    test_strchr_not_found();
    test_strchr_null_char();
    test_strrchr_found();
    test_strrchr_not_found();

    /* strstr */
    test_strstr_found();
    test_strstr_not_found();
    test_strstr_empty_needle();

    /* strcpy / strcat */
    test_strcpy_basic();
    test_strcpy_empty();
    test_strcat_basic();
    test_strcat_empty_dst();
    test_strcat_empty_src();

    /* snprintf */
    test_snprintf_basic();
    test_snprintf_int();
    test_snprintf_multiple();
    test_snprintf_truncation();

    printf("\n============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_run, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
