/*
 * test_libc.c — Host-side unit tests for kernel libc implementation
 *
 * Compiles against string.c, stdlib.c, printf.c on Linux x86_64 with gcc.
 * Tests pure-logic functions without kernel dependencies.
 *
 * This file does NOT include kernel headers directly (to avoid type conflicts
 * with standard headers).  Kernel function prototypes are declared manually.
 * Stub implementations are provided for kernel-specific symbols needed for
 * linking (vga_putchar, serial_putchar, libc_malloc, etc.).
 */

#include <stddef.h>   /* size_t */
#include <stdio.h>    /* printf */

/* ===================================================================
 *  Declarations of kernel libc functions being tested
 *
 *  These are defined in string.c, stdlib.c, or printf.c.
 *  Sizes/types match the kernel ABI (size_t → unsigned long long on
 *  x86_64, but ABI-compatible with unsigned long from <stddef.h>).
 * =================================================================== */

/* --- string.c --- */
extern size_t strlen(const char *s);
extern int    strcmp(const char *s1, const char *s2);
extern int    strncmp(const char *s1, const char *s2, size_t n);
extern void  *memcpy(void *dest, const void *src, size_t n);
extern int    memcmp(const void *s1, const void *s2, size_t n);
extern void  *memset(void *s, int c, size_t n);
extern char  *strcpy(char *dest, const char *src);
extern char  *strcat(char *dest, const char *src);
extern char  *strstr(const char *haystack, const char *needle);
extern char  *strchr(const char *s, int c);
extern char  *strtok(char *str, const char *delim);
extern long   strtol(const char *nptr, char **endptr, int base);

/* --- stdlib.c --- */
extern char  *itoa(int value, char *buf, int base);

/* --- printf.c --- */
extern int sprintf(char *buf, const char *fmt, ...);
extern int snprintf(char *buf, size_t n, const char *fmt, ...);

/* ===================================================================
 *  Stub implementations for kernel-specific functions
 *
 *  These are referenced by compiled kernel source code but not defined
 *  in any of the compiled files (they come from kernel-specific or
 *  assembly sources that we do not compile on host).
 * =================================================================== */

/* Called by kputchar() in printf.c (vga_putchar + serial_putchar) */
void vga_putchar(char c)     { (void)c; }
void serial_putchar(char c)  { (void)c; }

/* Called by malloc() inline → strdup() in stdlib.c */
void *libc_malloc(size_t sz) { (void)sz; return NULL; }

/* ===================================================================
 *  Implementations of static-inline kernel functions
 *
 *  The kernel headers define atoi, abs etc. as "static inline" in
 *  stdlib.h.  These are NOT emitted as external symbols during
 *  compilation of stdlib.c (they are only inlined, never called from
 *  stdlib.c itself).  We provide them here so the test binary can
 *  call them.
 * =================================================================== */

int atoi(const char *s) {
    return (int)strtol(s, (char **)0, 10);
}

int abs(int n) {
    return n < 0 ? -n : n;
}

/* ===================================================================
 *  Test framework
 * =================================================================== */

static int tests_run    = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)  do {                           \
    tests_run++;                                   \
    printf("  TEST: %-45s ... ", name);            \
} while (0)

#define PASS()      do {                           \
    tests_passed++;                                \
    printf("PASS\n");                              \
} while (0)

#define FAIL(msg)   do {                           \
    tests_failed++;                                \
    printf("FAIL\n");                              \
    printf("        %s\n", msg);                   \
} while (0)

#define ASSERT(cond, msg) do {                     \
    if (!(cond)) { FAIL(msg); return; }            \
} while (0)

#define ASSERT_STR_EQ(got, expected, msg) do {     \
    if (strcmp((got), (expected)) != 0) {          \
        tests_failed++;                            \
        printf("FAIL\n");                          \
        printf("        %s\n", msg);               \
        printf("        Expected: \"%s\"\n",       \
               (expected));                        \
        printf("        Got:      \"%s\"\n",       \
               (got));                             \
        return;                                    \
    }                                              \
} while (0)

#define ASSERT_INT_EQ(got, expected, msg) do {     \
    if ((got) != (expected)) {                     \
        tests_failed++;                            \
        printf("FAIL\n");                          \
        printf("        %s\n", msg);               \
        printf("        Expected: %d\n",           \
               (int)(expected));                   \
        printf("        Got:      %d\n",           \
               (int)(got));                        \
        return;                                    \
    }                                              \
} while (0)

#define ASSERT_LONG_EQ(got, expected, msg) do {    \
    if ((got) != (expected)) {                     \
        tests_failed++;                            \
        printf("FAIL\n");                          \
        printf("        %s\n", msg);               \
        printf("        Expected: %ld\n",          \
               (long)(expected));                  \
        printf("        Got:      %ld\n",          \
               (long)(got));                       \
        return;                                    \
    }                                              \
} while (0)

/* ===================================================================
 *  String function tests
 * =================================================================== */

static void test_strlen(void)
{
    TEST("strlen on empty string");
    ASSERT_INT_EQ(strlen(""), 0, "empty string should have length 0");
    PASS();

    TEST("strlen on normal string");
    ASSERT_INT_EQ(strlen("hello"), 5, "length of \"hello\"");
    PASS();

    TEST("strlen with spaces");
    ASSERT_INT_EQ(strlen("a b c"), 5, "length of \"a b c\"");
    PASS();

    TEST("strlen on long string");
    ASSERT_INT_EQ(strlen("abcdefghijklmnopqrstuvwxyz"), 26,
                  "length of alphabet");
    PASS();

    TEST("strlen on single char");
    ASSERT_INT_EQ(strlen("X"), 1, "length of \"X\"");
    PASS();
}

static void test_strcmp(void)
{
    TEST("strcmp equal strings");
    ASSERT_INT_EQ(strcmp("abc", "abc"), 0, "\"abc\" == \"abc\"");
    PASS();

    TEST("strcmp first < second (char diff)");
    ASSERT(strcmp("abc", "abd") < 0, "\"abc\" < \"abd\"");
    PASS();

    TEST("strcmp first > second (char diff)");
    ASSERT(strcmp("abd", "abc") > 0, "\"abd\" > \"abc\"");
    PASS();

    TEST("strcmp empty strings");
    ASSERT_INT_EQ(strcmp("", ""), 0, "empty strings equal");
    PASS();

    TEST("strcmp empty vs non-empty");
    ASSERT(strcmp("", "a") < 0, "\"\" < \"a\"");
    ASSERT(strcmp("a", "") > 0, "\"a\" > \"\"");
    PASS();

    TEST("strcmp case difference");
    ASSERT(strcmp("ABC", "abc") != 0, "\"ABC\" != \"abc\"");
    PASS();
}

static void test_strncmp(void)
{
    TEST("strncmp equal up to n");
    ASSERT_INT_EQ(strncmp("abc", "abc", 3), 0, "equal up to n=3");
    PASS();

    TEST("strncmp first < second within n");
    ASSERT(strncmp("abc", "abd", 3) < 0, "\"abc\" < \"abd\" with n=3");
    PASS();

    TEST("strncmp n=0 always 0");
    ASSERT_INT_EQ(strncmp("abc", "xyz", 0), 0, "n=0 should return 0");
    PASS();

    TEST("strncmp prefix equals with limited n");
    ASSERT_INT_EQ(strncmp("abc", "abcdef", 3), 0, "prefix match with n=3");
    PASS();

    TEST("strncmp null terminator stops at n");
    ASSERT_INT_EQ(strncmp("abc\0def", "abc\0xyz", 7), 0,
                  "null-terminated comparison");
    PASS();

    TEST("strncmp different at position n-1");
    ASSERT(strncmp("abx", "aby", 3) < 0, "diff at last position");
    PASS();
}

static void test_memcpy(void)
{
    TEST("memcpy basic copy");
    char src[] = "hello world";
    char dst[32] = {0};
    ASSERT(memcpy(dst, src, 12) == dst, "returns dest pointer");
    ASSERT_INT_EQ(memcmp(dst, src, 12), 0, "data matches");
    PASS();

    TEST("memcpy zero bytes");
    char buf[8] = "ABCDEFG";
    char orig[8];
    memcpy(orig, buf, 8);
    memcpy(buf, buf + 2, 0);
    ASSERT_INT_EQ(memcmp(buf, orig, 8), 0, "zero copy unchanged");
    PASS();

    TEST("memcpy partial copy");
    char partial_dst[8] = {0};
    char partial_src[] = "abcdef";
    memcpy(partial_dst, partial_src, 3);
    ASSERT_INT_EQ(memcmp(partial_dst, "abc", 3), 0, "first 3 bytes");
    PASS();
}

static void test_memcmp(void)
{
    TEST("memcmp equal buffers");
    ASSERT_INT_EQ(memcmp("abc", "abc", 3), 0, "equal buffers");
    PASS();

    TEST("memcmp different buffers");
    ASSERT(memcmp("abc", "abd", 3) != 0, "different buffers");
    PASS();

    TEST("memcmp zero length");
    ASSERT_INT_EQ(memcmp("abc", "xyz", 0), 0, "zero length equal");
    PASS();

    TEST("memcmp first byte differs");
    ASSERT(memcmp("xbc", "abc", 3) != 0, "first byte differs");
    PASS();

    TEST("memcmp returns signed diff");
    ASSERT(memcmp("abc", "bbc", 3) < 0, "'a' < 'b' returns negative");
    ASSERT(memcmp("bbc", "abc", 3) > 0, "'b' > 'a' returns positive");
    PASS();
}

static void test_memset(void)
{
    TEST("memset basic fill");
    char buf[16];
    memset(buf, 0xAA, 16);
    for (int i = 0; i < 16; i++)
        ASSERT((unsigned char)buf[i] == 0xAA, "all bytes set to 0xAA");
    PASS();

    TEST("memset zero fill");
    char zbuf[16];
    memset(zbuf, 1, 16);
    memset(zbuf, 0, 16);
    for (int i = 0; i < 16; i++)
        ASSERT(zbuf[i] == 0, "all bytes zeroed");
    PASS();

    TEST("memset partial fill");
    char pbuf[16];
    memset(pbuf, 0, 16);
    memset(pbuf + 4, 0xFF, 4);
    ASSERT((unsigned char)pbuf[3]  == 0,   "before region unchanged");
    ASSERT((unsigned char)pbuf[4]  == 0xFF, "region start set");
    ASSERT((unsigned char)pbuf[7]  == 0xFF, "region end set");
    ASSERT((unsigned char)pbuf[8]  == 0,    "after region unchanged");
    PASS();

    TEST("memset returns pointer");
    char rbuf[8];
    ASSERT(memset(rbuf, 0, 8) == rbuf, "returns s pointer");
    PASS();

    TEST("memset zero bytes no-op");
    char nbuf[8] = "test";
    memset(nbuf, 0xFF, 0);
    ASSERT_INT_EQ(memcmp(nbuf, "test", 5), 0, "zero bytes unchanged");
    PASS();
}

static void test_strcpy(void)
{
    TEST("strcpy basic copy");
    char dst[32];
    ASSERT(strcpy(dst, "hello") == dst, "returns dest");
    ASSERT_STR_EQ(dst, "hello", "copied string matches");
    PASS();

    TEST("strcpy empty string");
    char edst[8] = "xxxxx";
    strcpy(edst, "");
    ASSERT_STR_EQ(edst, "", "empty string copied");
    PASS();

    TEST("strcpy overwrites longer target");
    char odst[32] = "old content here";
    strcpy(odst, "new");
    ASSERT_STR_EQ(odst, "new", "overwrites with shorter string");
    PASS();

    TEST("strcpy null-terminates");
    char ndst[32];
    memset(ndst, 0xFF, sizeof(ndst));
    strcpy(ndst, "hi");
    ASSERT(ndst[2] == '\0', "null terminator written");
    PASS();
}

static void test_strcat(void)
{
    TEST("strcat basic concatenation");
    char dst[64] = "hello";
    ASSERT(strcat(dst, " world") == dst, "returns dest");
    ASSERT_STR_EQ(dst, "hello world", "concatenated string");
    PASS();

    TEST("strcat empty string");
    char edst[32] = "test";
    strcat(edst, "");
    ASSERT_STR_EQ(edst, "test", "appending empty string unchanged");
    PASS();

    TEST("strcat to empty dest");
    char zdst[32] = "";
    strcat(zdst, "hello");
    ASSERT_STR_EQ(zdst, "hello", "append to empty string");
    PASS();

    TEST("strcat multiple appends");
    char mdst[64] = "a";
    strcat(mdst, "b");
    strcat(mdst, "c");
    ASSERT_STR_EQ(mdst, "abc", "multiple concatenations");
    PASS();
}

static void test_strstr(void)
{
    TEST("strstr find at start");
    ASSERT(strstr("hello world", "hello") == (void *)"hello world",
           "needle at start");
    PASS();

    TEST("strstr find in middle");
    ASSERT_STR_EQ(strstr("hello world", "world"),
                  "world", "needle at end");
    PASS();

    TEST("strstr find in middle (substring)");
    ASSERT_STR_EQ(strstr("hello world", "lo wo"),
                  "lo world", "needle in middle");
    PASS();

    TEST("strstr not found returns NULL");
    ASSERT(strstr("hello", "xyz") == NULL, "needle not found");
    PASS();

    TEST("strstr empty needle returns haystack");
    ASSERT(strstr("hello", "") == (void *)"hello",
           "empty needle returns haystack");
    PASS();

    TEST("strstr single char match");
    ASSERT_STR_EQ(strstr("abcdef", "d"), "def", "single char in middle");
    PASS();

    TEST("strstr repeated pattern");
    ASSERT_STR_EQ(strstr("aaaa", "aa"), "aaaa", "overlapping match");
    PASS();
}

static void test_strchr(void)
{
    TEST("strchr find char at start");
    ASSERT(strchr("hello", 'h') == (void *)"hello", "char at start");
    PASS();

    TEST("strchr find char in middle");
    ASSERT_STR_EQ(strchr("hello", 'l'), "llo", "char in middle");
    PASS();

    TEST("strchr last occurrence");
    ASSERT_STR_EQ(strchr("hello", 'o'), "o", "char at end");
    PASS();

    TEST("strchr not found returns NULL");
    ASSERT(strchr("hello", 'z') == NULL, "char not found");
    PASS();

    TEST("strchr find null terminator");
    ASSERT(strchr("test", '\0') == (void *)("test" + 4),
           "null terminator found at end");
    PASS();
}

static void test_strtok(void)
{
    TEST("strtok single delimiter tokenization");
    char s1[] = "hello,world,test";
    ASSERT_STR_EQ(strtok(s1, ","), "hello", "first token");
    ASSERT_STR_EQ(strtok(NULL, ","), "world", "second token");
    ASSERT_STR_EQ(strtok(NULL, ","), "test", "third token");
    ASSERT(strtok(NULL, ",") == NULL, "no more tokens");
    PASS();

    TEST("strtok multiple consecutive delimiters");
    char s2[] = "a,,b,,,c";
    ASSERT_STR_EQ(strtok(s2, ","), "a", "first token no leading");
    ASSERT_STR_EQ(strtok(NULL, ","), "b", "skipped empty tokens");
    ASSERT_STR_EQ(strtok(NULL, ","), "c", "multiple delimiters skipped");
    ASSERT(strtok(NULL, ",") == NULL, "no more");
    PASS();

    TEST("strtok leading delimiters");
    char s3[] = ",,,hello";
    ASSERT_STR_EQ(strtok(s3, ","), "hello", "leading delimiters skipped");
    ASSERT(strtok(NULL, ",") == NULL, "no more");
    PASS();

    TEST("strtok no delimiters");
    char s4[] = "alone";
    ASSERT_STR_EQ(strtok(s4, ","), "alone", "single token");
    ASSERT(strtok(NULL, ",") == NULL, "no more");
    PASS();

    TEST("strtok multiple delimiters");
    char s5[] = "hello world\tfoo";
    ASSERT_STR_EQ(strtok(s5, " \t"), "hello", "space-delimited");
    ASSERT_STR_EQ(strtok(NULL, " \t"), "world", "tab skipped");
    ASSERT_STR_EQ(strtok(NULL, " \t"), "foo", "third token");
    ASSERT(strtok(NULL, " \t") == NULL, "no more");
    PASS();
}

/* ===================================================================
 *  stdlib function tests
 * =================================================================== */

static void test_atoi(void)
{
    TEST("atoi positive number");
    ASSERT_INT_EQ(atoi("123"), 123, "\"123\" -> 123");
    PASS();

    TEST("atoi negative number");
    ASSERT_INT_EQ(atoi("-456"), -456, "\"-456\" -> -456");
    PASS();

    TEST("atoi zero");
    ASSERT_INT_EQ(atoi("0"), 0, "\"0\" -> 0");
    PASS();

    TEST("atoi leading whitespace");
    ASSERT_INT_EQ(atoi("  42"), 42, "\"  42\" -> 42");
    PASS();

    TEST("atoi with sign");
    ASSERT_INT_EQ(atoi("+99"), 99, "\"+99\" -> 99");
    PASS();

    TEST("atoi stops at non-digit");
    ASSERT_INT_EQ(atoi("123abc"), 123, "\"123abc\" -> 123");
    PASS();

    TEST("atoi negative zero");
    ASSERT_INT_EQ(atoi("-0"), 0, "\"-0\" -> 0");
    PASS();

    TEST("atoi INT_MAX");
    ASSERT_INT_EQ(atoi("2147483647"), 2147483647,
                  "\"2147483647\" -> 2147483647");
    PASS();

    TEST("atoi INT_MIN");
    ASSERT_INT_EQ(atoi("-2147483648"), -2147483648,
                  "\"-2147483648\" -> -2147483648");
    PASS();
}

static void test_itoa(void)
{
    char buf[64];

    TEST("itoa zero");
    ASSERT_STR_EQ(itoa(0, buf, 10), "0", "zero in decimal");
    PASS();

    TEST("itoa positive decimal");
    ASSERT_STR_EQ(itoa(123, buf, 10), "123", "positive decimal");
    PASS();

    TEST("itoa negative decimal");
    ASSERT_STR_EQ(itoa(-123, buf, 10), "-123", "negative decimal");
    PASS();

    TEST("itoa hex");
    ASSERT_STR_EQ(itoa(255, buf, 16), "ff", "255 in hex");
    ASSERT_STR_EQ(itoa(0xABCD, buf, 16), "abcd", "0xABCD in hex");
    PASS();

    TEST("itoa binary");
    ASSERT_STR_EQ(itoa(42, buf, 2), "101010", "42 in binary");
    PASS();

    TEST("itoa octal");
    ASSERT_STR_EQ(itoa(64, buf, 8), "100", "64 in octal");
    PASS();

    TEST("itoa INT_MIN");
    ASSERT_STR_EQ(itoa(-2147483648, buf, 10), "-2147483648",
                  "INT_MIN in decimal");
    PASS();

    TEST("itoa INT_MAX");
    ASSERT_STR_EQ(itoa(2147483647, buf, 10), "2147483647",
                  "INT_MAX in decimal");
    PASS();

    TEST("itoa negative hex");
    ASSERT_STR_EQ(itoa(-255, buf, 16), "ffffff01",
                  "-255 in hex (unsigned conversion)");
    PASS();

    TEST("itoa base 36");
    ASSERT_STR_EQ(itoa(35, buf, 36), "z", "35 in base 36");
    ASSERT_STR_EQ(itoa(36, buf, 36), "10", "36 in base 36");
    PASS();

    TEST("itoa invalid base");
    ASSERT_STR_EQ(itoa(42, buf, 1), "", "base 1 returns empty");
    ASSERT_STR_EQ(itoa(42, buf, 37), "", "base 37 returns empty");
    PASS();
}

static void test_abs(void)
{
    TEST("abs positive");
    ASSERT_INT_EQ(abs(42), 42, "abs(42) -> 42");
    PASS();

    TEST("abs negative");
    ASSERT_INT_EQ(abs(-42), 42, "abs(-42) -> 42");
    PASS();

    TEST("abs zero");
    ASSERT_INT_EQ(abs(0), 0, "abs(0) -> 0");
    PASS();

    TEST("abs one");
    ASSERT_INT_EQ(abs(1), 1, "abs(1) -> 1");
    ASSERT_INT_EQ(abs(-1), 1, "abs(-1) -> 1");
    PASS();
}

/* ===================================================================
 *  sprintf / snprintf tests
 * =================================================================== */

static void test_sprintf_basic(void)
{
    char buf[256];

    TEST("sprintf plain string");
    sprintf(buf, "hello");
    ASSERT_STR_EQ(buf, "hello", "plain string");
    PASS();

    TEST("sprintf %s");
    sprintf(buf, "%s", "world");
    ASSERT_STR_EQ(buf, "world", "%s format");
    PASS();

    TEST("sprintf %d positive");
    sprintf(buf, "%d", (long long)42);
    ASSERT_STR_EQ(buf, "42", "%d positive");
    PASS();

    TEST("sprintf %d negative");
    sprintf(buf, "%d", (long long)(-42));
    ASSERT_STR_EQ(buf, "-42", "%d negative");
    PASS();

    TEST("sprintf %i");
    sprintf(buf, "%i", (long long)123);
    ASSERT_STR_EQ(buf, "123", "%i format");
    PASS();

    TEST("sprintf %u");
    sprintf(buf, "%u", (unsigned long long)3000000000u);
    ASSERT_STR_EQ(buf, "3000000000", "%u unsigned");
    PASS();

    TEST("sprintf %x");
    sprintf(buf, "%x", (unsigned long long)0xdead);
    ASSERT_STR_EQ(buf, "dead", "%x hex lowercase");
    PASS();

    TEST("sprintf %p");
    sprintf(buf, "%p", (void *)0x12345678UL);
    /* %p prints "0x" + 16 hex digits zero-padded */
    ASSERT(strlen(buf) == 18, "%p should be 18 chars (0x + 16 hex)");
    ASSERT(buf[0] == '0' && buf[1] == 'x', "%p starts with 0x");
    PASS();

    TEST("sprintf %c");
    sprintf(buf, "%c", 'A');
    ASSERT_STR_EQ(buf, "A", "%c character");
    PASS();

    TEST("sprintf %%");
    sprintf(buf, "%%");
    ASSERT_STR_EQ(buf, "%", "%% prints percent");
    PASS();
}

static void test_sprintf_padding(void)
{
    char buf[256];

    TEST("sprintf right-padded %5d");
    sprintf(buf, "%5d", (long long)42);
    ASSERT_STR_EQ(buf, "   42", "%5d right-padded with spaces");
    PASS();

    TEST("sprintf left-padded %-5d");
    sprintf(buf, "%-5d", (long long)42);
    ASSERT_STR_EQ(buf, "42   ", "%-5d left-padded with spaces");
    PASS();

    TEST("sprintf zero-padded %05d");
    sprintf(buf, "%05d", (long long)42);
    ASSERT_STR_EQ(buf, "00042", "%05d zero-padded");
    PASS();

    TEST("sprintf zero-padded on positive %08d");
    sprintf(buf, "%08d", (long long)123);
    ASSERT_STR_EQ(buf, "00000123", "%08d zero-padded 8 wide");
    PASS();

    TEST("sprintf hex padded %08x");
    sprintf(buf, "%08x", (unsigned long long)0xff);
    ASSERT_STR_EQ(buf, "000000ff", "%08x hex zero-padded");
    PASS();

    TEST("sprintf string padded %10s");
    sprintf(buf, "%10s", "hi");
    ASSERT_STR_EQ(buf, "        hi", "%10s right-padded");
    PASS();

    TEST("sprintf string left-padded %-10s");
    sprintf(buf, "%-10s", "hi");
    ASSERT_STR_EQ(buf, "hi        ", "%-10s left-padded");
    PASS();
}

static void test_sprintf_multiple(void)
{
    char buf[256];

    TEST("sprintf multiple formats");
    sprintf(buf, "%s %d %x", "test", (long long)255, (unsigned long long)0xff);
    ASSERT_STR_EQ(buf, "test 255 ff",
                  "%s %d %x combined");
    PASS();

    TEST("sprintf mixed text and formats");
    sprintf(buf, "val=%d, name=%s", (long long)10, "alice");
    ASSERT_STR_EQ(buf, "val=10, name=alice",
                  "mixed text and formats");
    PASS();

    TEST("sprintf empty string in %%s");
    sprintf(buf, "[%s]", "");
    ASSERT_STR_EQ(buf, "[]", "empty string in %%s");
    PASS();

    TEST("sprintf NULL in %%s");
    sprintf(buf, "%s", (const char *)NULL);
    ASSERT_STR_EQ(buf, "(null)", "NULL string becomes \"(null)\"");
    PASS();
}

static void test_snprintf(void)
{
    char buf[64];

    TEST("snprintf basic truncation");
    memset(buf, 0xAA, sizeof(buf));
    snprintf(buf, 8, "hello world this is long");
    ASSERT(strlen(buf) <= 7, "snprintf truncated to n-1");
    ASSERT_STR_EQ(buf, "hello w", "snprintf first 7 chars");
    PASS();

    TEST("snprintf exact fit");
    snprintf(buf, 5, "test");
    ASSERT_STR_EQ(buf, "test", "exact fit within buffer");
    PASS();

    TEST("snprintf empty buffer");
    buf[0] = 'X';
    snprintf(buf, 1, "hello");
    ASSERT(buf[0] == '\0', "n=1 gives empty string");
    PASS();

    TEST("snprintf null buf is no-op");
    /* vsnprintf checks for NULL or n==0 */
    ASSERT_INT_EQ(snprintf(NULL, 0, "test"), 0,
                  "NULL/0 returns 0");
    PASS();
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== host_libc unit tests ===\n");
    printf("Platform: Linux x86_64 (host)\n\n");

    /* --- string.h tests --- */
    printf("--- string.h ---\n");
    test_strlen();
    test_strcmp();
    test_strncmp();
    test_memcpy();
    test_memcmp();
    test_memset();
    test_strcpy();
    test_strcat();
    test_strstr();
    test_strchr();
    test_strtok();

    /* --- stdlib.h tests --- */
    printf("\n--- stdlib.h ---\n");
    test_atoi();
    test_itoa();
    test_abs();

    /* --- printf tests --- */
    printf("\n--- printf.h ---\n");
    test_sprintf_basic();
    test_sprintf_padding();
    test_sprintf_multiple();
    test_snprintf();

    /* --- Summary --- */
    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    if (tests_failed == 0)
        printf("All tests passed.\n");

    return tests_failed > 0 ? 1 : 0;
}
