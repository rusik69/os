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
#include <stdint.h>   /* uint8_t */
#include <stdio.h>    /* printf */
#include <limits.h>   /* INT_MAX, INT_MIN, LONG_MAX, LONG_MIN */
#include <stdarg.h>  /* va_list for vsnprintf */

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
extern void  *memmove(void *dest, const void *src, size_t n);
extern int    memcmp(const void *s1, const void *s2, size_t n);
extern void  *memset(void *s, int c, size_t n);
extern void  *memchr(const void *s, int c, size_t n);
extern char  *strcpy(char *dest, const char *src);
extern char  *strncpy(char *dest, const char *src, size_t n);
extern char  *strcat(char *dest, const char *src);
extern char  *strncat(char *dest, const char *src, size_t n);
extern char  *strstr(const char *haystack, const char *needle);
extern char  *strchr(const char *s, int c);
extern char  *strrchr(const char *s, int c);
extern char  *strtok(char *str, const char *delim);
extern char  *strtok_r(char *str, const char *delim, char **saveptr);
extern char  *strsep(char **stringp, const char *delim);
extern size_t strspn(const char *s, const char *accept);
extern size_t strcspn(const char *s, const char *reject);
extern char  *strpbrk(const char *s, const char *accept);
extern long   strtol(const char *nptr, char **endptr, int base);
extern unsigned long strtoul(const char *nptr, char **endptr, int base);
extern size_t strnlen(const char *s, size_t maxlen);
extern void  *memccpy(void *dest, const void *src, int c, size_t n);

/* --- stdlib.c --- */
extern char  *itoa(int value, char *buf, int base);
extern char  *ltoa(long value, char *buf, int base);
extern char  *strdup(const char *s);

/* qsort / bsearch / rand / srand */
extern void   qsort(void *base, size_t nmemb, size_t size,
                    int (*compar)(const void *, const void *));
extern void  *bsearch(const void *key, const void *base, size_t nmemb,
                      size_t size,
                      int (*compar)(const void *, const void *));
extern void   srand(unsigned int seed);
extern int    rand(void);

/* string.c (beyond line 270) */
extern char  *strtrim(char *s);

#ifndef RAND_MAX
#define RAND_MAX 0x7FFFFFFF
#endif

/* --- printf.c --- */
extern int sprintf(char *buf, const char *fmt, ...);
extern int snprintf(char *buf, size_t n, const char *fmt, ...);
extern int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

/* ---- string_ext.c functions (extended string ops) ---- */
extern size_t strlcat(char *dst, const char *src, size_t size);
extern size_t strlcpy(char *dst, const char *src, size_t size);
extern int    strcasecmp(const char *s1, const char *s2);
extern int    strncasecmp(const char *s1, const char *s2, size_t n);
extern char  *strchrnul(const char *s, int c);
extern char  *strcasestr(const char *haystack, const char *needle);
extern const char *strsignal(int signum);
extern void  *memmem(const void *haystack, size_t haystacklen,
                     const void *needle, size_t needlelen);

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
/* Stubs are in stubs.c — see stubs.c for libc_malloc, libc_free, etc. */

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

#define ASSERT_PTR_EQ(got, expected, msg) do {     \
    if ((got) != (expected)) {                     \
        tests_failed++;                            \
        printf("FAIL\n");                          \
        printf("        %s\n", msg);               \
        printf("        Expected: %p\n",           \
               (void *)(expected));                \
        printf("        Got:      %p\n",           \
               (void *)(got));                     \
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

    TEST("strcmp long equal strings");
    ASSERT_INT_EQ(strcmp("abcdefghijklmnopqrstuvwxyz0123456789",
                         "abcdefghijklmnopqrstuvwxyz0123456789"),
                  0, "long equal strings");
    PASS();

    TEST("strcmp diff at very end of long string");
    ASSERT(strcmp("abcdefghijklmnopqrstuvwxyz0123456789",
                  "abcdefghijklmnopqrstuvwxyz0123456788") > 0,
           "diff at last char");
    PASS();

    TEST("strcmp with embedded null bytes");
    /* Both strings have null at position 2, should compare equal up to null */
    ASSERT_INT_EQ(strcmp("ab\0cd", "ab\0ef"), 0,
                  "null bytes cause early termination (equal)");
    PASS();

    TEST("strcmp null byte vs different char");
    ASSERT(strcmp("ab\0cd", "abc\0") != 0,
           "null vs char differ after common prefix");
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

    TEST("strncmp n larger than strings");
    ASSERT_INT_EQ(strncmp("ab", "ab", 10), 0, "n larger than both strings");
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

    TEST("memcpy large copy");
    char large_src[4096];
    char large_dst[4096];
    for (int i = 0; i < 4096; i++) large_src[i] = (char)(i & 0xFF);
    memset(large_dst, 0, 4096);
    memcpy(large_dst, large_src, 4096);
    ASSERT_INT_EQ(memcmp(large_dst, large_src, 4096), 0,
                  "large 4096-byte copy matches");
    PASS();

    TEST("memcpy single byte");
    char one_dst[4] = {0};
    char one_src = 'X';
    memcpy(one_dst, &one_src, 1);
    ASSERT_INT_EQ(one_dst[0], 'X', "single byte copied");
    ASSERT_INT_EQ(one_dst[1], 0, "rest unchanged");
    PASS();

    TEST("memcpy returns dest pointer");
    char ret_dst[16];
    ASSERT(memcpy(ret_dst, "test", 5) == ret_dst, "returns dest pointer");
    PASS();
}

static void test_memmove(void)
{
    TEST("memmove non-overlapping copy");
    char buf1[32] = "hello world";
    char dest1[32] = {0};
    ASSERT(memmove(dest1, buf1, 12) == dest1, "returns dest");
    ASSERT_INT_EQ(memcmp(dest1, buf1, 12), 0, "data matches");
    PASS();

    TEST("memmove overlapping forward (dest < src)");
    char ovfwd[32] = "abcdefghij";
    memmove(ovfwd + 2, ovfwd, 6);
    ASSERT_INT_EQ(memcmp(ovfwd, "ababcdef", 8), 0,
                  "forward overlap: dest < src");
    PASS();

    TEST("memmove overlapping backward (dest > src)");
    char ovbwd[32] = "abcdefghij";
    memmove(ovbwd, ovbwd + 2, 6);
    ASSERT_INT_EQ(memcmp(ovbwd, "cdefgh", 6), 0,
                  "backward overlap: dest > src");
    PASS();

    TEST("memmove same pointer");
    char same[16] = "test data";
    char same_orig[16];
    memcpy(same_orig, same, 10);
    memmove(same, same, 10);
    ASSERT_INT_EQ(memcmp(same, same_orig, 10), 0,
                  "same pointer no change");
    PASS();

    TEST("memmove zero bytes");
    char zbuf[16] = "preserved";
    char zorig[16];
    memcpy(zorig, zbuf, 10);
    memmove(zbuf + 2, zbuf, 0);
    ASSERT_INT_EQ(memcmp(zbuf, zorig, 10), 0, "zero bytes no change");
    PASS();

    TEST("memmove exact overlap identical");
    char exov[16] = "identical";
    char exov_orig[16];
    memcpy(exov_orig, exov, 10);
    memmove(exov, exov, 10);
    ASSERT_INT_EQ(memcmp(exov, exov_orig, 10), 0,
                  "exact same region unchanged");
    PASS();

    TEST("memmove huge overlap (4096 bytes)");
    {
        char huge[4096];
        for (int i = 0; i < 4096; i++) huge[i] = (char)(i & 0xFF);
        /* Move bytes 0..2047 to offset 2048 (overlapping forward) */
        memmove(huge + 2048, huge, 2048);
        int ok = 1;
        for (int i = 0; i < 2048; i++)
            if ((unsigned char)huge[2048 + i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
        ASSERT(ok, "forward huge overlap copied correctly");
    }
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

    TEST("memcmp last byte differs");
    ASSERT(memcmp("abc", "abd", 3) != 0, "last byte differs");
    ASSERT(memcmp("abc", "abd", 3) < 0, "'c' < 'd' returns negative");
    PASS();

    TEST("memcmp single byte");
    ASSERT_INT_EQ(memcmp("a", "a", 1), 0, "single byte equal");
    ASSERT(memcmp("a", "b", 1) < 0, "single byte diff negative");
    PASS();

    TEST("memcmp all zeros vs all zeros");
    char zeros1[16] = {0};
    char zeros2[16] = {0};
    ASSERT_INT_EQ(memcmp(zeros1, zeros2, 16), 0, "all zeros equal");
    PASS();

    TEST("memcmp all 0xFF vs all 0xFF");
    char ff1[16];
    char ff2[16];
    memset(ff1, 0xFF, 16);
    memset(ff2, 0xFF, 16);
    ASSERT_INT_EQ(memcmp(ff1, ff2, 16), 0, "all 0xFF buffers equal");
    PASS();

    TEST("memcmp 0xFF vs zero buffer");
    ASSERT(memcmp(ff1, zeros1, 16) != 0, "all 0xFF vs all zero differ");
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

    TEST("memset large fill");
    char large[4096];
    memset(large, 0x55, 4096);
    for (int i = 0; i < 4096; i++)
        ASSERT((unsigned char)large[i] == 0x55, "all 4096 bytes = 0x55");
    PASS();

    TEST("memset pattern fill 0xAB");
    char pat[64];
    memset(pat, 0xAB, 64);
    for (int i = 0; i < 64; i++)
        ASSERT((unsigned char)pat[i] == 0xAB, "all 64 bytes = 0xAB");
    PASS();

    TEST("memset fill with char value -1 (0xFF)");
    char neg[32];
    memset(neg, -1, 32);
    for (int i = 0; i < 32; i++)
        ASSERT((unsigned char)neg[i] == 0xFF, "all bytes = 0xFF");
    PASS();

    TEST("memset n=1 single byte");
    char one[4] = {0};
    memset(one, 'Z', 1);
    ASSERT_INT_EQ(one[0], 'Z', "first byte set");
    ASSERT_INT_EQ(one[1], 0, "second byte unchanged");
    PASS();

    TEST("memset n=4095 (non-aligned)");
    {
        char big[4096];
        memset(big, 0, 4096);
        memset(big, 0xAB, 4095);
        int ok = 1;
        for (int i = 0; i < 4095; i++)
            if ((unsigned char)big[i] != 0xAB) { ok = 0; break; }
        ASSERT(ok, "first 4095 bytes = 0xAB");
        ASSERT((unsigned char)big[4095] == 0, "last byte untouched");
    }
    PASS();
}

static void test_memchr(void)
{
    TEST("memchr find char in middle");
    const char *mcs = "hello world";
    ASSERT(memchr(mcs, 'w', 11) == (void *)(mcs + 6), "found 'w' at offset 6");
    PASS();

    TEST("memchr char not found");
    ASSERT(memchr("hello", 'z', 5) == NULL, "'z' not found");
    PASS();

    TEST("memchr zero length");
    ASSERT(memchr("hello", 'h', 0) == NULL, "zero length returns NULL");
    PASS();

    TEST("memchr find first char");
    ASSERT(memchr("abcde", 'a', 5) == (void *)("abcde"),
           "first char at start");
    PASS();

    TEST("memchr find last char");
    ASSERT(memchr("abcde", 'e', 5) == (void *)("abcde" + 4),
           "last char at end");
    PASS();

    TEST("memchr null byte in buffer");
    char nb[] = "ab\0cd";
    ASSERT(memchr(nb, '\0', 5) == (void *)(nb + 2), "null byte found");
    PASS();

    TEST("memchr find byte by numeric value");
    unsigned char nbuf[] = {0x00, 0x10, 0x20, 0x30, 0x40};
    ASSERT(memchr(nbuf, 0x20, 5) == (void *)(nbuf + 2),
           "found 0x20 at offset 2");
    PASS();

    TEST("memchr find 0xFF byte");
    unsigned char ffbuf[] = {0x00, 0xFF, 0xAA, 0xFF, 0x55};
    ASSERT(memchr(ffbuf, 0xFF, 5) == (void *)(ffbuf + 1),
           "found 0xFF at offset 1");
    ASSERT(memchr(ffbuf + 2, 0xFF, 3) == (void *)(ffbuf + 3),
           "found second 0xFF at offset 3");
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

    TEST("strcpy long string");
    char ldst[256];
    const char *lsrc = "abcdefghijklmnopqrstuvwxyz0123456789";
    strcpy(ldst, lsrc);
    ASSERT_STR_EQ(ldst, lsrc, "long string copied");
    PASS();

    TEST("strcpy exact buffer");
    char exdst[4];
    strcpy(exdst, "abc");
    ASSERT_STR_EQ(exdst, "abc", "exact 3-char string fits");
    PASS();
}

static void test_strncpy(void)
{
    TEST("strncpy basic copy");
    char dst[16];
    memset(dst, 'X', sizeof(dst));
    strncpy(dst, "hello", 16);
    ASSERT_STR_EQ(dst, "hello", "basic copy");
    PASS();

    TEST("strncpy null padding");
    char npad[8];
    memset(npad, 'X', sizeof(npad));
    strncpy(npad, "hi", 8);
    ASSERT_INT_EQ(npad[0], 'h', "first char");
    ASSERT_INT_EQ(npad[1], 'i', "second char");
    ASSERT_INT_EQ(npad[2], '\0', "null-padded start");
    ASSERT_INT_EQ(npad[7], '\0', "null-padded to end");
    PASS();

    TEST("strncpy truncation (n < src length)");
    {
        char trunc[4];
        memset(trunc, 'X', sizeof(trunc));
        (void)strncpy(trunc, "hello world", 4);
        ASSERT_INT_EQ(memcmp(trunc, "hell", 4), 0,
                      "all 4 chars from source copied, no null-term");
    }
    PASS();

    TEST("strncpy exact fit");
    char exact[4];
    memset(exact, 'X', sizeof(exact));
    strncpy(exact, "abc", 4);
    ASSERT_INT_EQ(exact[0], 'a', "first char");
    ASSERT_INT_EQ(exact[3], '\0', "null-padded to n");
    PASS();

    TEST("strncpy empty source");
    char empty[8];
    memset(empty, 'X', sizeof(empty));
    strncpy(empty, "", 8);
    for (int i = 0; i < 8; i++)
        ASSERT_INT_EQ(empty[i], '\0', "all bytes nulled");
    PASS();

    TEST("strncpy n=0 no-op");
    {
        char noop[16] = "unchanged";
        (void)strncpy(noop, "new", 0);
        ASSERT_STR_EQ(noop, "unchanged", "n=0 leaves buffer unchanged");
    }
    PASS();

    TEST("strncpy returns dest");
    char retdst[16];
    ASSERT(strncpy(retdst, "test", 16) == retdst, "returns dest pointer");
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

    TEST("strcat long string");
    char ldst[256] = "start";
    strcat(ldst, "-middle");
    strcat(ldst, "-end");
    ASSERT_STR_EQ(ldst, "start-middle-end", "long concatenation");
    PASS();

    TEST("strcat single char multiple times");
    char sdst[32] = "";
    strcat(sdst, "x");
    strcat(sdst, "x");
    strcat(sdst, "x");
    ASSERT_STR_EQ(sdst, "xxx", "three single-char appends");
    PASS();

    TEST("strcat to full buffer (no space)");
    {
        char full[4] = "abc";
        /* Appending to an already-full buffer (no null terminator at position 3)
         * strcat should write at position 3, but buffer is only 4 bytes including null */
        strcat(full, "");
        ASSERT_STR_EQ(full, "abc", "appending empty to full buffer unchanged");
    }
    PASS();
}

static void test_strncat(void)
{
    TEST("strncat basic");
    char dst[32] = "hello";
    ASSERT(strncat(dst, " world", 7) == dst, "returns dest");
    ASSERT_STR_EQ(dst, "hello world", "basic concatenation");
    PASS();

    TEST("strncat truncation");
    char trunc[16] = "ab";
    strncat(trunc, "cdefghijklmnop", 4);
    ASSERT_STR_EQ(trunc, "abcdef", "copied up to 4 chars + null");
    PASS();

    TEST("strncat n=0");
    char n0[32] = "test";
    strncat(n0, "extra", 0);
    ASSERT_STR_EQ(n0, "test", "n=0 does nothing");
    PASS();

    TEST("strncat empty source");
    char esrc[32] = "hello";
    strncat(esrc, "", 10);
    ASSERT_STR_EQ(esrc, "hello", "empty src unchanged");
    PASS();

    TEST("strncat to empty dest");
    char edst[32] = "";
    strncat(edst, "hi", 3);
    ASSERT_STR_EQ(edst, "hi", "append to empty dest");
    PASS();

    TEST("strncat returns dest");
    char retdst[32] = "x";
    ASSERT(strncat(retdst, "y", 2) == retdst, "returns dest pointer");
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

    TEST("strstr empty haystack");
    ASSERT(strstr("", "a") == NULL, "empty haystack returns NULL");
    ASSERT(strstr("", "") == (void *)"", "empty haystack + empty needle returns haystack");
    PASS();

    TEST("strstr needle longer than haystack");
    ASSERT(strstr("abc", "abcdef") == NULL,
           "needle longer than haystack returns NULL");
    PASS();

    TEST("strstr multiple occurrences");
    ASSERT_STR_EQ(strstr("ababab", "aba"), "ababab",
                  "first occurrence of multiple");
    PASS();

    TEST("strstr pattern of 5 a's in long string of a's");
    {
        char long_as[256];
        memset(long_as, 'a', 255);
        long_as[255] = '\0';
        const char *found = strstr(long_as, "aaaaa");
        ASSERT(found == (void *)long_as, "found 5 a's at start of 255 a's");
    }
    PASS();

    TEST("strstr single char not found in long 'a's");
    {
        char long_as2[101];
        memset(long_as2, 'a', 100);
        long_as2[100] = '\0';
        ASSERT(strstr(long_as2, "b") == NULL, "'b' not found in all 'a's");
    }
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

    TEST("strchr empty string");
    ASSERT(strchr("", 'a') == NULL, "empty string no char");
    ASSERT(strchr("", '\0') == (void *)"", "empty string null terminator");
    PASS();

    TEST("strchr multiple occurrences returns first");
    ASSERT_STR_EQ(strchr("banana", 'a'), "anana",
                  "first 'a' in banana");
    PASS();

    TEST("strchr char numeric value");
    ASSERT(strchr("abc", 0x62) == (void *)("abc" + 1), "0x62 is 'b'");
    PASS();
}

static void test_strrchr(void)
{
    TEST("strrchr last occurrence");
    ASSERT_STR_EQ(strrchr("hello", 'l'), "lo", "last 'l' in hello");
    PASS();

    TEST("strrchr single occurrence");
    ASSERT_STR_EQ(strrchr("abcde", 'c'), "cde", "single 'c' in middle");
    PASS();

    TEST("strrchr not found returns NULL");
    ASSERT(strrchr("hello", 'z') == NULL, "'z' not found");
    PASS();

    TEST("strrchr null terminator");
    ASSERT(strrchr("test", '\0') == (void *)("test" + 4),
           "null terminator at end");
    PASS();

    TEST("strrchr first char is last");
    ASSERT_STR_EQ(strrchr("aaaa", 'a'), "a", "last of repeated chars");
    PASS();

    TEST("strrchr char at start");
    ASSERT(strrchr("hello", 'h') == (void *)"hello", "only 'h' at start");
    PASS();

    TEST("strrchr empty string");
    ASSERT(strrchr("", 'a') == NULL, "empty string no char");
    ASSERT(strrchr("", '\0') == (void *)"", "empty string null terminator");
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

    TEST("strtok all delimiters");
    char s6[] = ",,,";
    ASSERT(strtok(s6, ",") == NULL, "all delimiters returns NULL");
    PASS();

    TEST("strtok empty string");
    char s7[] = "";
    ASSERT(strtok(s7, ",") == NULL, "empty string returns NULL");
    PASS();

    TEST("strtok single char token");
    char s8[] = "a";
    ASSERT_STR_EQ(strtok(s8, ","), "a", "single char token");
    ASSERT(strtok(NULL, ",") == NULL, "no more");
    PASS();

    TEST("strtok multiple different delimiters");
    {
        char s9[] = "hello,world;foo|bar";
        ASSERT_STR_EQ(strtok(s9, ",;|"), "hello", "first token with multiple delimiters");
        ASSERT_STR_EQ(strtok(NULL, ",;|"), "world", "second token");
        ASSERT_STR_EQ(strtok(NULL, ",;|"), "foo", "third token");
        ASSERT_STR_EQ(strtok(NULL, ",;|"), "bar", "fourth token");
        ASSERT(strtok(NULL, ",;|") == NULL, "no more tokens");
    }
    PASS();
}

static void test_strtok_r(void)
{
    TEST("strtok_r basic");
    char s1[] = "hello,world,test";
    char *save1;
    ASSERT_STR_EQ(strtok_r(s1, ",", &save1), "hello", "first token");
    ASSERT_STR_EQ(strtok_r(NULL, ",", &save1), "world", "second token");
    ASSERT_STR_EQ(strtok_r(NULL, ",", &save1), "test", "third token");
    ASSERT(strtok_r(NULL, ",", &save1) == NULL, "no more tokens");
    PASS();

    TEST("strtok_r interleaved state");
    char s2[] = "a,b,c";
    char s3[] = "1:2:3";
    char *sp2, *sp3;
    ASSERT_STR_EQ(strtok_r(s2, ",", &sp2), "a", "first s2 token");
    ASSERT_STR_EQ(strtok_r(s3, ":", &sp3), "1", "first s3 token");
    ASSERT_STR_EQ(strtok_r(NULL, ",", &sp2), "b", "second s2 token");
    ASSERT_STR_EQ(strtok_r(NULL, ":", &sp3), "2", "second s3 token");
    ASSERT_STR_EQ(strtok_r(NULL, ",", &sp2), "c", "third s2 token");
    ASSERT_STR_EQ(strtok_r(NULL, ":", &sp3), "3", "third s3 token");
    ASSERT(strtok_r(NULL, ",", &sp2) == NULL, "s2 done");
    ASSERT(strtok_r(NULL, ":", &sp3) == NULL, "s3 done");
    PASS();

    TEST("strtok_r empty string");
    char s4[] = "";
    char *sp4;
    ASSERT(strtok_r(s4, ",", &sp4) == NULL, "empty string returns NULL");
    PASS();

    TEST("strtok_r consecutive delimiters");
    char s5[] = "x,,y";
    char *sp5;
    ASSERT_STR_EQ(strtok_r(s5, ",", &sp5), "x", "first");
    ASSERT_STR_EQ(strtok_r(NULL, ",", &sp5), "y", "skips empty");
    ASSERT(strtok_r(NULL, ",", &sp5) == NULL, "done");
    PASS();

    TEST("strtok_r saveptr preserved after exhaustion");
    {
        char s6[] = "a,b";
        char *sp6;
        strtok_r(s6, ",", &sp6);
        strtok_r(NULL, ",", &sp6);
        /* saveptr should now point to null terminator */
        ASSERT(*sp6 == '\0', "saveptr at null terminator after token exhaustion");
    }
    PASS();
}

static void test_strsep(void)
{
    TEST("strsep basic");
    char s1[] = "hello,world,test";
    char *sp1 = s1;
    ASSERT_STR_EQ(strsep(&sp1, ","), "hello", "first token");
    ASSERT_STR_EQ(strsep(&sp1, ","), "world", "second token");
    ASSERT_STR_EQ(strsep(&sp1, ","), "test", "third token");
    ASSERT(strsep(&sp1, ",") == NULL, "no more tokens");
    PASS();

    TEST("strsep consecutive delimiters");
    char s2[] = "a,,b";
    char *sp2 = s2;
    ASSERT_STR_EQ(strsep(&sp2, ","), "a", "first");
    ASSERT_STR_EQ(strsep(&sp2, ","), "", "empty token between delimiters");
    ASSERT_STR_EQ(strsep(&sp2, ","), "b", "last");
    ASSERT(strsep(&sp2, ",") == NULL, "done");
    PASS();

    TEST("strsep leading delimiters");
    char s3[] = ",,x";
    char *sp3 = s3;
    ASSERT_STR_EQ(strsep(&sp3, ","), "", "leading empty");
    ASSERT_STR_EQ(strsep(&sp3, ","), "", "second leading empty");
    ASSERT_STR_EQ(strsep(&sp3, ","), "x", "token after");
    ASSERT(strsep(&sp3, ",") == NULL, "done");
    PASS();

    TEST("strsep with NULL stringp");
    {
        char *null_sp = NULL;
        ASSERT(strsep(&null_sp, ",") == NULL, "NULL stringp returns NULL");
    }
    PASS();
}

static void test_strspn(void)
{
    TEST("strspn basic");
    ASSERT_INT_EQ(strspn("hello", "he"), 2, "first 2 chars 'h','e' match");
    PASS();

    TEST("strspn full string match");
    ASSERT_INT_EQ(strspn("abc", "abc"), 3, "all chars match");
    PASS();

    TEST("strspn no match");
    ASSERT_INT_EQ(strspn("xyz", "abc"), 0, "no chars match");
    PASS();

    TEST("strspn empty string");
    ASSERT_INT_EQ(strspn("", "abc"), 0, "empty string returns 0");
    PASS();

    TEST("strspn accept all printable");
    ASSERT_INT_EQ(strspn("hello", "abcdefghijklmnopqrstuvwxyz"), 5,
                  "all lowercase letters accept");
    PASS();

    TEST("strspn empty accept string");
    ASSERT_INT_EQ(strspn("hello", ""), 0, "empty accept returns 0");
    ASSERT_INT_EQ(strspn("", ""), 0, "both empty returns 0");
    PASS();
}

static void test_strcspn(void)
{
    TEST("strcspn basic");
    ASSERT_INT_EQ(strcspn("hello", "o"), 4, "'o' at position 4");
    PASS();

    TEST("strcspn no reject chars");
    ASSERT_INT_EQ(strcspn("abc", "xyz"), 3, "no chars to reject");
    PASS();

    TEST("strcspn first char matches");
    ASSERT_INT_EQ(strcspn("hello", "h"), 0, "first char matches");
    PASS();

    TEST("strcspn empty string");
    ASSERT_INT_EQ(strcspn("", "abc"), 0, "empty string returns 0");
    PASS();

    TEST("strcspn char not found");
    ASSERT_INT_EQ(strcspn("abc", "d"), 3, "not found returns full length");
    PASS();
}

static void test_strpbrk(void)
{
    TEST("strpbrk found");
    ASSERT(strpbrk("hello world", "ow") == (void *)("hello world" + 4),
           "finds 'o' first");
    PASS();

    TEST("strpbrk not found");
    ASSERT(strpbrk("hello", "xyz") == NULL, "no chars found");
    PASS();

    TEST("strpbrk first matching char");
    ASSERT(strpbrk("abcdef", "cba") == (void *)("abcdef"),
           "'a' matches first (char 'a' in set)");
    ASSERT(strpbrk("abcdef", "fed") == (void *)("abcdef" + 3),
           "'d' matches first at offset 3");
    PASS();

    TEST("strpbrk empty accept");
    ASSERT(strpbrk("hello", "") == NULL, "empty accept returns NULL");
    PASS();

    TEST("strpbrk empty string");
    ASSERT(strpbrk("", "abc") == NULL, "empty string returns NULL");
    PASS();
}

/* ===================================================================
 *  strnlen / memccpy tests (new in string.c)
 * =================================================================== */

static void test_strnlen(void)
{
    TEST("strnlen normal string");
    {
        char s[] = "hello";
        ASSERT_INT_EQ(strnlen(s, 10), 5, "strnlen within maxlen");
    }
    PASS();

    TEST("strnlen truncated by maxlen");
    ASSERT_INT_EQ(strnlen("hello world", 5), 5, "truncated at maxlen");
    PASS();

    TEST("strnlen empty string");
    {
        char s[] = "";
        ASSERT_INT_EQ(strnlen(s, 10), 0, "empty string");
    }
    PASS();

    TEST("strnlen zero maxlen");
    ASSERT_INT_EQ(strnlen("hello", 0), 0, "zero maxlen returns 0");
    PASS();

    TEST("strnlen exact fit");
    ASSERT_INT_EQ(strnlen("abc", 3), 3, "exact length match");
    PASS();

    TEST("strnlen null bytes not counted");
    {
        char nb[] = "ab\0cdef";
        ASSERT_INT_EQ(strnlen(nb, 10), 2, "stops at null byte");
    }
    PASS();

    TEST("strnlen maxlen < actual length");
    ASSERT_INT_EQ(strnlen("abcdefghij", 4), 4, "maxlen smaller than string");
    PASS();

    TEST("strnlen very large maxlen");
    {
        char s[] = "hi";
        ASSERT_INT_EQ(strnlen(s, 999999), 2, "large maxlen capped at strlen");
    }
    PASS();
}

static void test_memccpy(void)
{
    TEST("memccpy copy until char found");
    char dst1[32] = {0};
    const char *src1 = "hello world";
    void *result1 = memccpy(dst1, src1, 'w', 32);
    ASSERT(result1 == (void *)(dst1 + 7), "returns ptr after 'w'");
    ASSERT_INT_EQ(memcmp(dst1, "hello w", 7), 0, "copied up to 'w'");
    PASS();

    TEST("memccpy char not found returns NULL");
    char dst2[32] = {0};
    void *result2 = memccpy(dst2, "hello", 'z', 32);
    ASSERT(result2 == NULL, "char not found returns NULL");
    ASSERT_INT_EQ(memcmp(dst2, "hello", 6), 0, "all bytes copied even without match");
    PASS();

    TEST("memccpy zero bytes");
    char dst3[32] = "unchanged";
    char orig3[32];
    memcpy(orig3, dst3, 10);
    void *result3 = memccpy(dst3, "new", 'x', 0);
    ASSERT(result3 == NULL, "zero length returns NULL");
    ASSERT_INT_EQ(memcmp(dst3, orig3, 10), 0, "no change with n=0");
    PASS();

    TEST("memccpy first byte matches");
    char dst4[32] = {0};
    void *result4 = memccpy(dst4, "abc", 'a', 32);
    ASSERT(result4 == (void *)(dst4 + 1), "first byte matches, returns ptr after");
    ASSERT_INT_EQ(dst4[0], 'a', "first byte copied");
    ASSERT_INT_EQ(dst4[1], 0, "stopped after first byte");
    PASS();

    TEST("memccpy last byte matches");
    char dst5[32] = {0};
    void *result5 = memccpy(dst5, "abcdef", 'f', 32);
    ASSERT(result5 == (void *)(dst5 + 6), "last byte matches");
    ASSERT_INT_EQ(memcmp(dst5, "abcdef", 7), 0, "all bytes copied");
    PASS();

    TEST("memccpy null byte in middle");
    char dst6[32] = {0};
    unsigned char src6[] = {'a', 'b', '\0', 'c', 'd'};
    void *result6 = memccpy(dst6, src6, '\0', 5);
    ASSERT(result6 == (void *)(dst6 + 3), "null byte found at offset 2, return ptr after");
    ASSERT_INT_EQ((unsigned char)dst6[0], 'a', "first byte");
    ASSERT_INT_EQ((unsigned char)dst6[1], 'b', "second byte");
    ASSERT_INT_EQ((unsigned char)dst6[2], '\0', "null byte copied");
    PASS();

    TEST("memccpy n smaller than match position");
    char dst7[8] = {0};
    memset(dst7, 0xAA, 4);
    void *result7 = memccpy(dst7, "abcdef", 'f', 3);
    ASSERT(result7 == NULL, "n too small, char not found");
    ASSERT_INT_EQ(memcmp(dst7, "abc", 3), 0, "first 3 bytes copied");
    PASS();
}

/* ===================================================================
 *  Extended string function tests (string_ext.c)
 * =================================================================== */

static void test_strlcpy_func(void)
{
    TEST("strlcpy basic copy");
    char buf[16];
    memset(buf, 'X', sizeof(buf));
    size_t ret = strlcpy(buf, "hello", sizeof(buf));
    ASSERT_INT_EQ((int)ret, 5, "returns strlen of src");
    ASSERT_STR_EQ(buf, "hello", "copied string");
    ASSERT_INT_EQ((unsigned char)buf[10], 'X', "rest of buffer unchanged");
    PASS();

    TEST("strlcpy truncation");
    char tbuf[4];
    memset(tbuf, 'X', sizeof(tbuf));
    size_t tret = strlcpy(tbuf, "hello world", sizeof(tbuf));
    ASSERT_INT_EQ((int)tret, 11, "returns strlen of src (not truncated)");
    ASSERT_INT_EQ(memcmp(tbuf, "hel", 3), 0, "truncated to 3 chars");
    ASSERT_INT_EQ((unsigned char)tbuf[3], '\0', "null-terminated");
    PASS();

    TEST("strlcpy exact fit");
    char fbuf[4];
    memset(fbuf, 'X', sizeof(fbuf));
    strlcpy(fbuf, "abc", sizeof(fbuf));
    ASSERT_STR_EQ(fbuf, "abc", "exact fit");
    PASS();

    TEST("strlcpy size=0");
    char zbuf[16] = "unchanged";
    size_t zret = strlcpy(zbuf, "new", 0);
    ASSERT_INT_EQ((int)zret, 3, "returns strlen of src");
    ASSERT_STR_EQ(zbuf, "unchanged", "buffer unchanged with size=0");
    PASS();

    TEST("strlcpy empty src");
    char ebuf[16] = "XXXXXXXXXXXXX";
    strlcpy(ebuf, "", sizeof(ebuf));
    ASSERT_STR_EQ(ebuf, "", "empty src produces empty string");
    PASS();
}

static void test_strlcat_func(void)
{
    TEST("strlcat basic concatenation");
    char buf[16] = "hello";
    size_t ret = strlcat(buf, " world", sizeof(buf));
    ASSERT_INT_EQ((int)ret, 11, "returns total length (5+6)");
    ASSERT_STR_EQ(buf, "hello world", "concatenated");
    PASS();

    TEST("strlcat truncation");
    char tbuf[8] = "ab";
    size_t tret = strlcat(tbuf, "cdefghijklmnop", sizeof(tbuf));
    ASSERT_INT_EQ((int)tret, 16, "returns theoretical total length (2+14)");
    ASSERT_STR_EQ(tbuf, "abcdefg", "truncated to 7 chars (2+5) with null");
    PASS();

    TEST("strlcat to empty dest");
    char ebuf[16] = "";
    strlcat(ebuf, "hello", sizeof(ebuf));
    ASSERT_STR_EQ(ebuf, "hello", "append to empty dest");
    PASS();

    TEST("strlcat size=0");
    char zbuf[16] = "test";
    size_t zret = strlcat(zbuf, "extra", 0);
    ASSERT_INT_EQ((int)zret, 5, "returns strlen(src) when size=0");
    ASSERT_STR_EQ(zbuf, "test", "buffer unchanged with size=0");
    PASS();

    TEST("strlcat full buffer (no space)");
    char fbuf[4] = "abc";
    size_t fret = strlcat(fbuf, "d", sizeof(fbuf));
    ASSERT_INT_EQ((int)fret, 4, "returns total length");
    ASSERT(strlen(fbuf) <= 3, "buffer does not overflow");
    PASS();
}

static void test_strcasecmp_func(void)
{
    TEST("strcasecmp equal strings");
    ASSERT_INT_EQ(strcasecmp("hello", "hello"), 0, "identical strings");
    PASS();

    TEST("strcasecmp case difference");
    ASSERT_INT_EQ(strcasecmp("HELLO", "hello"), 0, "case-insensitive equal");
    PASS();

    TEST("strcasecmp mixed case");
    ASSERT_INT_EQ(strcasecmp("HeLLo", "hElLo"), 0, "mixed case equal");
    PASS();

    TEST("strcasecmp different strings");
    ASSERT(strcasecmp("abc", "abd") < 0, "\"abc\" < \"abd\"");
    ASSERT(strcasecmp("abd", "abc") > 0, "\"abd\" > \"abc\"");
    PASS();

    TEST("strcasecmp different case");
    ASSERT(strcasecmp("ABC", "abd") < 0, "ABC < abd (case-insensitive)");
    PASS();

    TEST("strcasecmp empty strings");
    ASSERT_INT_EQ(strcasecmp("", ""), 0, "empty strings");
    PASS();

    TEST("strcasecmp string with prefix");
    ASSERT(strcasecmp("hello", "HELLO, WORLD") < 0, "shorter string < longer (case-insensitive)");
    PASS();
}

static void test_strncasecmp_func(void)
{
    TEST("strncasecmp equal strings");
    ASSERT_INT_EQ(strncasecmp("hello", "hello", 5), 0, "identical strings");
    PASS();

    TEST("strncasecmp case difference within n");
    ASSERT_INT_EQ(strncasecmp("HELLO", "hello", 5), 0, "case-insensitive within n");
    PASS();

    TEST("strncasecmp n=0");
    ASSERT_INT_EQ(strncasecmp("abc", "xyz", 0), 0, "n=0 returns 0");
    PASS();

    TEST("strncasecmp prefix match");
    ASSERT_INT_EQ(strncasecmp("HELLO", "hello, world", 5), 0, "prefix match first 5 chars");
    PASS();

    TEST("strncasecmp different case, different string");
    ASSERT(strncasecmp("ABC", "abb", 3) > 0, "ABC > abb case-insensitive");
    PASS();

    TEST("strncasecmp different within n");
    ASSERT(strncasecmp("abc", "abd", 3) < 0, "abc < abd at n=3");
    PASS();
}

static void test_strchrnul_func(void)
{
    TEST("strchrnul char found");
    ASSERT(strchrnul("hello", 'e') == (void *)("hello" + 1),
           "finds 'e' at offset 1");
    PASS();

    TEST("strchrnul char not found returns end pointer");
    ASSERT(strchrnul("hello", 'z') == (void *)("hello" + 5),
           "not found returns pointer to null terminator");
    PASS();

    TEST("strchrnul empty string");
    ASSERT(strchrnul("", 'a') == (void *)(""),
           "empty string returns pointer to null");
    ASSERT(strchrnul("", '\0') == (void *)(""),
           "empty string null byte returns start");
    PASS();

    TEST("strchrnul null terminator");
    ASSERT(strchrnul("test", '\0') == (void *)("test" + 4),
           "null terminator found at end");
    PASS();

    TEST("strchrnul first char matches");
    ASSERT(strchrnul("abc", 'a') == (void *)("abc"),
           "first char matches, returned");
    PASS();
}

static void test_strcasestr_func(void)
{
    TEST("strcasestr find at start");
    ASSERT(strcasestr("hello world", "HELLO") == (void *)"hello world",
           "case-insensitive find at start");
    PASS();

    TEST("strcasestr find in middle");
    ASSERT_STR_EQ(strcasestr("hello WORLD", "world"),
                  "WORLD", "case-insensitive find in middle");
    PASS();

    TEST("strcasestr not found");
    ASSERT(strcasestr("hello", "xyz") == NULL, "needle not found");
    PASS();

    TEST("strcasestr empty needle");
    ASSERT(strcasestr("hello", "") == (void *)"hello",
           "empty needle returns haystack");
    PASS();

    TEST("strcasestr mixed case needle");
    ASSERT_STR_EQ(strcasestr("Hello World", "woRlD"),
                  "World", "mixed case needle");
    PASS();

    TEST("strcasestr empty haystack");
    ASSERT(strcasestr("", "a") == NULL, "empty haystack returns NULL");
    ASSERT(strcasestr("", "") == (void *)"", "both empty returns haystack");
    PASS();

    TEST("strcasestr repeated pattern");
    ASSERT_STR_EQ(strcasestr("abABab", "aba"), "abABab",
                  "first occurrence of multiple, case-insensitive");
    PASS();
}

/* ── strsignal tests ───────────────────────────────────────────── */

static void test_strsignal_func(void)
{
    TEST("strsignal known signal");
    ASSERT(strcmp(strsignal(2), "SIGINT") == 0, "signal 2 = SIGINT");
    PASS();

    TEST("strsignal SIGHUP");
    ASSERT(strcmp(strsignal(1), "SIGHUP") == 0, "signal 1 = SIGHUP");
    PASS();

    TEST("strsignal SIGKILL");
    ASSERT(strcmp(strsignal(9), "SIGKILL") == 0, "signal 9 = SIGKILL");
    PASS();

    TEST("strsignal SIGSYS");
    ASSERT(strcmp(strsignal(31), "SIGSYS") == 0, "signal 31 = SIGSYS");
    PASS();

    TEST("strsignal SIGRTMIN");
    ASSERT(strcmp(strsignal(32), "Unknown signal") == 0, "signal 32+ = unknown");
    PASS();

    TEST("strsignal invalid signal");
    ASSERT(strcmp(strsignal(0), "Unknown signal") == 0, "signal 0 = unknown");
    ASSERT(strcmp(strsignal(99), "Unknown signal") == 0, "signal 99 = unknown");
    PASS();
}

/* ── memmem tests ──────────────────────────────────────────────── */

static void test_memmem_func(void)
{
    const char *haystack = "hello world, welcome to the kernel unit tests";

    TEST("memmem find at start");
    ASSERT(memmem(haystack, strlen(haystack), "hello", 5) == (void *)haystack,
           "find at start");
    PASS();

    TEST("memmem find in middle");
    const char *found = (const char *)memmem(haystack, strlen(haystack),
                                              "welcome", 7);
    ASSERT(found != NULL && strcmp(found, "welcome to the kernel unit tests") == 0,
           "find in middle");
    PASS();

    TEST("memmem not found");
    ASSERT(memmem(haystack, strlen(haystack), "xyzzy", 5) == NULL,
           "needle not found");
    PASS();

    TEST("memmem empty needle");
    ASSERT(memmem(haystack, strlen(haystack), "", 0) == (void *)haystack,
           "empty needle returns haystack");
    PASS();

    TEST("memmem needle larger than haystack");
    ASSERT(memmem(haystack, 5, "hello world", 11) == NULL,
           "needle larger returns NULL");
    PASS();

    TEST("memmem binary data");
    uint8_t data[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t needle[] = {0x02, 0x03};
    void *result = memmem(data, sizeof(data), needle, sizeof(needle));
    ASSERT(result == (void *)(data + 2), "find binary substring");
    PASS();

    TEST("memmem exact match");
    ASSERT(memmem(data, sizeof(data), data, sizeof(data)) == (void *)data,
           "exact match of whole buffer");
    PASS();

    TEST("memmem null haystack");
    ASSERT(memmem(NULL, 10, "hello", 5) == NULL, "null haystack returns NULL");
    PASS();

    TEST("memmem null needle");
    ASSERT(memmem("hello", 5, NULL, 5) == NULL, "null needle returns NULL");
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

static void test_strtol_func(void)
{
    char *endptr;

    TEST("strtol base 10");
    ASSERT_LONG_EQ(strtol("12345", &endptr, 10), 12345, "base 10 parse");
    ASSERT(*endptr == '\0', "endptr at end for base 10");
    PASS();

    TEST("strtol base 16");
    ASSERT_LONG_EQ(strtol("1a2b", &endptr, 16), 0x1a2b, "base 16 parse");
    ASSERT(*endptr == '\0', "endptr at end for base 16");
    PASS();

    TEST("strtol base 16 uppercase");
    ASSERT_LONG_EQ(strtol("FF", &endptr, 16), 255, "base 16 uppercase");
    PASS();

    TEST("strtol base 8");
    ASSERT_LONG_EQ(strtol("777", &endptr, 8), 511, "base 8 parse");
    ASSERT(*endptr == '\0', "endptr at end for base 8");
    PASS();

    TEST("strtol base 2");
    ASSERT_LONG_EQ(strtol("101010", &endptr, 2), 42, "base 2 parse");
    ASSERT(*endptr == '\0', "endptr at end for base 2");
    PASS();

    TEST("strtol negative");
    ASSERT_LONG_EQ(strtol("-42", &endptr, 10), -42, "negative decimal");
    ASSERT(*endptr == '\0', "endptr at end for negative");
    PASS();

    TEST("strtol positive sign");
    ASSERT_LONG_EQ(strtol("+99", &endptr, 10), 99, "positive sign");
    PASS();

    TEST("strtol base 0 auto-detect hex");
    ASSERT_LONG_EQ(strtol("0xFF", &endptr, 0), 255, "0x prefix -> base 16");
    PASS();

    TEST("strtol base 0 auto-detect octal");
    ASSERT_LONG_EQ(strtol("077", &endptr, 0), 63, "leading 0 -> base 8");
    PASS();

    TEST("strtol base 0 auto-detect decimal");
    ASSERT_LONG_EQ(strtol("42", &endptr, 0), 42, "no prefix -> base 10");
    PASS();

    TEST("strtol base 16 with 0x prefix");
    ASSERT_LONG_EQ(strtol("0xABC", &endptr, 16), 0xABC,
                   "base 16 with explicit 0x");
    PASS();

    TEST("strtol invalid input (no digits)");
    ASSERT_LONG_EQ(strtol("abc", &endptr, 10), 0,
                   "no valid digits -> 0");
    ASSERT(endptr == (void *)"abc", "endptr points to start");
    PASS();

    TEST("strtol large valid within range");
    ASSERT_LONG_EQ(strtol("2147483647", &endptr, 10), 2147483647L,
                   "INT_MAX in decimal works");
    ASSERT_LONG_EQ(strtol("-2147483648", &endptr, 10), -2147483648L,
                   "INT_MIN in decimal works");
    PASS();

    TEST("strtol 10-digit value");
    ASSERT_LONG_EQ(strtol("9999999999", &endptr, 10), 9999999999L,
                   "large 10-digit value fits in long");
    PASS();

    TEST("strtol negative large");
    ASSERT_LONG_EQ(strtol("-9999999999", &endptr, 10), -9999999999L,
                   "negative large value");
    PASS();

    TEST("strtol whitespace");
    ASSERT_LONG_EQ(strtol("   -123", &endptr, 10), -123,
                   "leading whitespace consumed");
    PASS();

    TEST("strtol stops at invalid char");
    ASSERT_LONG_EQ(strtol("123xyz", &endptr, 10), 123,
                   "stops at 'x'");
    ASSERT(*endptr == 'x', "endptr at invalid char");
    PASS();

    TEST("strtol just '0x' prefix with no digits");
    ASSERT_LONG_EQ(strtol("0x", &endptr, 0), 0, "'0x' alone returns 0");
    PASS();

    TEST("strtol whitespace with sign");
    ASSERT_LONG_EQ(strtol("  - 5", &endptr, 10), 0,
                   "'  - 5' has sign not directly before digit, returns 0");
    PASS();

    TEST("strtol empty string");
    ASSERT_LONG_EQ(strtol("", &endptr, 10), 0, "empty string returns 0");
    ASSERT(endptr == (void *)"", "endptr at start for empty string");
    PASS();

    TEST("strtol just '+'");
    ASSERT_LONG_EQ(strtol("+", &endptr, 10), 0, "just '+' returns 0");
    PASS();

    TEST("strtol just '-'");
    ASSERT_LONG_EQ(strtol("-", &endptr, 10), 0, "just '-' returns 0");
    PASS();
}

static void test_strtoul_func(void)
{
    char *endptr;

    TEST("strtoul base 10");
    ASSERT_LONG_EQ((long)strtoul("3000000000", &endptr, 10),
                   (long)3000000000UL, "large unsigned decimal");
    PASS();

    TEST("strtoul base 16");
    ASSERT_LONG_EQ((long)strtoul("DEADBEEF", &endptr, 16),
                   (long)0xDEADBEEF, "large hex value");
    PASS();

    TEST("strtoul auto hex");
    ASSERT_LONG_EQ((long)strtoul("0x100", &endptr, 0),
                   (long)256, "auto-detect hex");
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

    TEST("itoa INT_MIN in binary");
    ASSERT_STR_EQ(itoa(-2147483648, buf, 2), "10000000000000000000000000000000",
                  "INT_MIN binary = 1 followed by 31 zeros");
    PASS();
}

static void test_ltoa(void)
{
    char buf[128];

    TEST("ltoa zero");
    ASSERT_STR_EQ(ltoa(0, buf, 10), "0", "ltoa zero");
    PASS();

    TEST("ltoa positive");
    ASSERT_STR_EQ(ltoa(123456789L, buf, 10), "123456789",
                  "ltoa positive decimal");
    PASS();

    TEST("ltoa negative");
    ASSERT_STR_EQ(ltoa(-123456789L, buf, 10), "-123456789",
                  "ltoa negative decimal");
    PASS();

    TEST("ltoa LONG_MAX");
    ASSERT_STR_EQ(ltoa(9223372036854775807L, buf, 10),
                  "9223372036854775807", "ltoa LONG_MAX");
    PASS();

    TEST("ltoa LONG_MIN");
    ASSERT_STR_EQ(ltoa(-9223372036854775807L - 1L, buf, 10),
                  "-9223372036854775808", "ltoa LONG_MIN");
    PASS();

    TEST("ltoa hex");
    ASSERT_STR_EQ(ltoa(255L, buf, 16), "ff", "ltoa 255 in hex");
    PASS();

    TEST("ltoa binary base 2");
    ASSERT_STR_EQ(ltoa(42L, buf, 2), "101010", "ltoa 42 in binary");
    PASS();

    TEST("ltoa octal base 8");
    ASSERT_STR_EQ(ltoa(64L, buf, 8), "100", "ltoa 64 in octal");
    PASS();

    TEST("ltoa base 36");
    ASSERT_STR_EQ(ltoa(35L, buf, 36), "z", "ltoa 35 in base 36");
    ASSERT_STR_EQ(ltoa(71L, buf, 36), "1z", "ltoa 71 in base 36");
    PASS();
}

static void test_strdup_func(void)
{
    TEST("strdup basic");
    char *dup = strdup("hello world");
    ASSERT(dup != NULL, "strdup returns non-NULL");
    ASSERT_STR_EQ(dup, "hello world", "duplicate matches");
    __builtin_free(dup);
    PASS();

    TEST("strdup empty string");
    char *edup = strdup("");
    ASSERT(edup != NULL, "empty strdup returns non-NULL");
    ASSERT_STR_EQ(edup, "", "empty duplicate");
    __builtin_free(edup);
    PASS();

    TEST("strdup long string");
    char *ldup = strdup("abcdefghijklmnopqrstuvwxyz0123456789");
    ASSERT(ldup != NULL, "long strdup returns non-NULL");
    ASSERT_STR_EQ(ldup, "abcdefghijklmnopqrstuvwxyz0123456789",
                  "long duplicate matches");
    __builtin_free(ldup);
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

    TEST("snprintf truncation with %d");
    char dbuf[16];
    memset(dbuf, 0xAA, sizeof(dbuf));
    snprintf(dbuf, 5, "%d", (long long)123456);
    ASSERT_STR_EQ(dbuf, "1234", "%d truncated to 4 chars");
    PASS();

    TEST("snprintf truncation with %s");
    char sbuf[16];
    memset(sbuf, 0xAA, sizeof(sbuf));
    snprintf(sbuf, 5, "%s", "abcdefgh");
    ASSERT_STR_EQ(sbuf, "abcd", "%s truncated to 4 chars");
    PASS();

    TEST("snprintf n=2");
    char n2buf[4] = "xxx";
    snprintf(n2buf, 2, "hello");
    ASSERT_STR_EQ(n2buf, "h", "n=2 copies 1 char");
    PASS();

    TEST("snprintf large format string");
    char lbuf[256];
    snprintf(lbuf, 256, "%s %d %x %u %c %s",
             "hello", (long long)42, (unsigned long long)0xFF,
             (unsigned long long)100, '!', "world");
    ASSERT_STR_EQ(lbuf, "hello 42 ff 100 ! world",
                  "multiple format specifiers");
    PASS();

    TEST("snprintf zero buffer n=0");
    char zbuf[4] = "xxx";
    snprintf(zbuf, 0, "hello");
    ASSERT_STR_EQ(zbuf, "xxx", "n=0 leaves buffer unchanged");
    PASS();
}

/* ===================================================================
 *  Advanced printf format specifiers
 * =================================================================== */

static void test_sprintf_advanced(void)
{
    char buf[256];

    TEST("sprintf %%ld long decimal");
    snprintf(buf, sizeof(buf), "%ld", 123456789L);
    ASSERT_STR_EQ(buf, "123456789", "%ld format");
    PASS();

    TEST("sprintf %%lu long unsigned");
    snprintf(buf, sizeof(buf), "%lu", 3000000000UL);
    ASSERT_STR_EQ(buf, "3000000000", "%lu format");
    PASS();

    TEST("sprintf %%llx long long hex");
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)0xdeadbeef);
    ASSERT_STR_EQ(buf, "deadbeef", "%llx format");
    PASS();

    TEST("sprintf %%x preserves width with %%08x");
    snprintf(buf, sizeof(buf), "%08x", (unsigned long long)0xff);
    ASSERT_STR_EQ(buf, "000000ff", "%08x zero-padded hex");
    PASS();

    TEST("sprintf %%-10d left justify int");
    snprintf(buf, sizeof(buf), "%-10d", (long long)42);
    ASSERT(strlen(buf) == 10, "%%-10d produces 10 chars");
    ASSERT(buf[0] == '4' && buf[1] == '2', "%%-10d has '42' at start");
    PASS();

    TEST("sprintf %%010d zero pad positive");
    snprintf(buf, sizeof(buf), "%010d", (long long)42);
    ASSERT_STR_EQ(buf, "0000000042", "%010d zero-padded");
    PASS();

    TEST("sprintf %%010d zero pad negative");
    snprintf(buf, sizeof(buf), "%010d", (long long)(-42));
    ASSERT(strlen(buf) >= 8, "%%010d negative has reasonable length");
    ASSERT(buf[0] == '-', "%%010d negative has sign first");
    PASS();

    TEST("snprintf with %%d truncation");
    char sbuf[8];
    snprintf(sbuf, 5, "%d", 123456789);
    ASSERT_STR_EQ(sbuf, "1234", "truncated %%d");
    PASS();

    TEST("snprintf with %%x truncation");
    snprintf(sbuf, 6, "%x", 0xdeadbeef);
    ASSERT_STR_EQ(sbuf, "deadb", "truncated %%x (5 chars + null)");
    PASS();

    TEST("sprintf %%d zero value");
    snprintf(buf, sizeof(buf), "%d", 0);
    ASSERT_STR_EQ(buf, "0", "zero printed correctly");
    PASS();

    TEST("sprintf %%05d with zero value");
    snprintf(buf, sizeof(buf), "%05d", 0);
    ASSERT_STR_EQ(buf, "00000", "zero padded with 5 zeros");
    PASS();
}

/* ===================================================================
 *  qsort tests
 * =================================================================== */

static int cmp_int(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

static int is_sorted_int(int *arr, size_t n)
{
    for (size_t i = 1; i < n; i++)
        if (arr[i-1] > arr[i]) return 0;
    return 1;
}

typedef struct { int id; char name[16]; } test_rec_t;

static int cmp_rec_id(const void *a, const void *b)
{
    int ia = ((const test_rec_t *)a)->id;
    int ib = ((const test_rec_t *)b)->id;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

static void test_qsort(void)
{
    printf("\n[qsort]\n");

    TEST("qsort empty (n=0 no-op)");
    qsort(NULL, 0, sizeof(int), cmp_int);
    PASS();

    TEST("qsort single element");
    {
        int a[] = { 42 };
        qsort(a, 1, sizeof(int), cmp_int);
        ASSERT_INT_EQ(a[0], 42, "single element unchanged");
    }
    PASS();

    TEST("qsort 2 already sorted");
    {
        int a[] = { 1, 2 };
        qsort(a, 2, sizeof(int), cmp_int);
        ASSERT_INT_EQ(a[0], 1, "first");
        ASSERT_INT_EQ(a[1], 2, "second");
    }
    PASS();

    TEST("qsort 2 reverse sorted");
    {
        int a[] = { 2, 1 };
        qsort(a, 2, sizeof(int), cmp_int);
        ASSERT_INT_EQ(a[0], 1, "first after sort");
        ASSERT_INT_EQ(a[1], 2, "second after sort");
    }
    PASS();

    TEST("qsort 8 already sorted");
    {
        int a[] = { 10, 20, 30, 40, 50, 60, 70, 80 };
        qsort(a, 8, sizeof(int), cmp_int);
        ASSERT(is_sorted_int(a, 8), "8 sorted stays sorted");
        ASSERT_INT_EQ(a[0], 10, "first preserved");
        ASSERT_INT_EQ(a[7], 80, "last preserved");
    }
    PASS();

    TEST("qsort 8 reverse sorted");
    {
        int a[] = { 80, 70, 60, 50, 40, 30, 20, 10 };
        qsort(a, 8, sizeof(int), cmp_int);
        ASSERT(is_sorted_int(a, 8), "8 reverse becomes sorted");
        ASSERT_INT_EQ(a[0], 10, "smallest first");
        ASSERT_INT_EQ(a[7], 80, "largest last");
    }
    PASS();

    TEST("qsort 8 all equal");
    {
        int a[] = { 7, 7, 7, 7, 7, 7, 7, 7 };
        qsort(a, 8, sizeof(int), cmp_int);
        ASSERT(is_sorted_int(a, 8), "all equal sorted");
        ASSERT_INT_EQ(a[0], 7, "first");
        ASSERT_INT_EQ(a[4], 7, "middle");
    }
    PASS();

    TEST("qsort 100 sorted");
    {
        int a[100];
        for (int i = 0; i < 100; i++) a[i] = i * 10;
        qsort(a, 100, sizeof(int), cmp_int);
        ASSERT(is_sorted_int(a, 100), "100 sorted stays sorted");
        ASSERT_INT_EQ(a[0], 0, "first");
        ASSERT_INT_EQ(a[99], 990, "last");
    }
    PASS();

    TEST("qsort 100 reverse sorted");
    {
        int a[100];
        for (int i = 0; i < 100; i++) a[i] = (99 - i) * 10;
        qsort(a, 100, sizeof(int), cmp_int);
        ASSERT(is_sorted_int(a, 100), "100 reverse becomes sorted");
        ASSERT_INT_EQ(a[0], 0, "first");
        ASSERT_INT_EQ(a[99], 990, "last");
    }
    PASS();

    TEST("qsort struct sort by id");
    {
        test_rec_t recs[] = { {3, "charlie"}, {1, "alice"}, {2, "bob"} };
        qsort(recs, 3, sizeof(test_rec_t), cmp_rec_id);
        ASSERT_INT_EQ(recs[0].id, 1, "first by id");
        ASSERT_INT_EQ(recs[1].id, 2, "second by id");
        ASSERT_INT_EQ(recs[2].id, 3, "third by id");
    }
    PASS();
}

/* ===================================================================
 *  bsearch tests
 * =================================================================== */

static void test_bsearch(void)
{
    printf("\n[bsearch]\n");

    {
        int arr[] = { 10, 20, 30, 40, 50 };
        size_t n = 5;

        TEST("bsearch key present (first)");
        {
            int key = 10;
            int *found = (int *)bsearch(&key, arr, n, sizeof(int), cmp_int);
            ASSERT_PTR_EQ(found, &arr[0], "found first element");
        }
        PASS();

        TEST("bsearch key present (middle)");
        {
            int key = 30;
            int *found = (int *)bsearch(&key, arr, n, sizeof(int), cmp_int);
            ASSERT_PTR_EQ(found, &arr[2], "found middle element");
        }
        PASS();

        TEST("bsearch key present (last)");
        {
            int key = 50;
            int *found = (int *)bsearch(&key, arr, n, sizeof(int), cmp_int);
            ASSERT_PTR_EQ(found, &arr[4], "found last element");
        }
        PASS();

        TEST("bsearch key absent (too small)");
        {
            int key = 5;
            ASSERT(bsearch(&key, arr, n, sizeof(int), cmp_int) == NULL,
                   "key less than all returns NULL");
        }
        PASS();

        TEST("bsearch key absent (too large)");
        {
            int key = 99;
            ASSERT(bsearch(&key, arr, n, sizeof(int), cmp_int) == NULL,
                   "key greater than all returns NULL");
        }
        PASS();

        TEST("bsearch key absent (in middle)");
        {
            int key = 25;
            ASSERT(bsearch(&key, arr, n, sizeof(int), cmp_int) == NULL,
                   "key in middle gap returns NULL");
        }
        PASS();
    }

    TEST("bsearch empty array (n=0)");
    {
        int key = 1;
        int *arr = NULL;
        ASSERT(bsearch(&key, arr, 0, sizeof(int), cmp_int) == NULL,
               "empty array returns NULL");
    }
    PASS();

    TEST("bsearch single element found");
    {
        int arr[] = { 42 };
        int key = 42;
        int *found = (int *)bsearch(&key, arr, 1, sizeof(int), cmp_int);
        ASSERT_PTR_EQ(found, &arr[0], "single element found");
    }
    PASS();

    TEST("bsearch single element not found");
    {
        int arr[] = { 42 };
        int key = 99;
        ASSERT(bsearch(&key, arr, 1, sizeof(int), cmp_int) == NULL,
               "single element not found returns NULL");
    }
    PASS();

    TEST("bsearch zero nmemb");
    {
        int arr[] = { 1, 2, 3 };
        int key = 1;
        ASSERT(bsearch(&key, arr, 0, sizeof(int), cmp_int) == NULL,
               "zero nmemb returns NULL");
    }
    PASS();
}

/* ===================================================================
 *  rand / srand tests
 * =================================================================== */

static void test_rand(void)
{
    printf("\n[rand/srand]\n");

    TEST("srand(12345) deterministic first 5 values");
    {
        srand(12345);
        int a1 = rand(), a2 = rand(), a3 = rand(), a4 = rand(), a5 = rand();
        srand(12345);
        ASSERT_INT_EQ(rand(), a1, "first value matches");
        ASSERT_INT_EQ(rand(), a2, "second value matches");
        ASSERT_INT_EQ(rand(), a3, "third value matches");
        ASSERT_INT_EQ(rand(), a4, "fourth value matches");
        ASSERT_INT_EQ(rand(), a5, "fifth value matches");
    }
    PASS();

    TEST("srand(0) and srand(1) produce different sequences");
    {
        srand(0);
        int v0 = rand();
        srand(1);
        int v1 = rand();
        ASSERT(v0 != v1, "different seeds produce different first values");
    }
    PASS();

    TEST("rand() produces values in [0, RAND_MAX]");
    {
        srand(42);
        int r1 = rand();
        int r2 = rand();
        int r3 = rand();
        ASSERT(r1 >= 0 && r1 <= RAND_MAX, "rand() value 1 in range");
        ASSERT(r2 >= 0 && r2 <= RAND_MAX, "rand() value 2 in range");
        ASSERT(r3 >= 0 && r3 <= RAND_MAX, "rand() value 3 in range");
    }
    PASS();

    TEST("RAND_MAX meets POSIX minimum (>= 32767)");
    {
        ASSERT(RAND_MAX >= 32767, "RAND_MAX >= 32767");
    }
    PASS();

    TEST("reseeding reproduces entire 10-element sequence");
    {
        srand(9999);
        int seq[10];
        for (int i = 0; i < 10; i++) seq[i] = rand();
        srand(9999);
        for (int i = 0; i < 10; i++)
            ASSERT_INT_EQ(rand(), seq[i], "sequence matches after reseed");
    }
    PASS();

    TEST("rand() values not trivially constant");
    {
        srand(54321);
        int r1 = rand();
        int r2 = rand();
        int r3 = rand();
        ASSERT(!(r1 == r2 && r2 == r3), "first 3 values differ");
    }
    PASS();

    TEST("1000 rand() calls stay in [0, RAND_MAX]");
    {
        srand(7777);
        int ok = 1;
        for (int i = 0; i < 1000; i++) {
            int r = rand();
            if (r < 0 || r > RAND_MAX) { ok = 0; break; }
        }
        ASSERT(ok, "all 1000 values in valid range");
    }
    PASS();
}

/* ===================================================================
 *  strtrim tests
 * =================================================================== */

static void test_strtrim(void)
{
    printf("\n[strtrim]\n");

    TEST("strtrim empty string");
    {
        char s[] = "";
        ASSERT_STR_EQ(strtrim(s), "", "empty string stays empty");
    }
    PASS();

    TEST("strtrim no whitespace");
    {
        char s[] = "hello";
        ASSERT_STR_EQ(strtrim(s), "hello", "no whitespace unchanged");
    }
    PASS();

    TEST("strtrim leading spaces only");
    {
        char s[] = "   hello";
        ASSERT_STR_EQ(strtrim(s), "hello", "leading spaces trimmed");
    }
    PASS();

    TEST("strtrim trailing spaces only");
    {
        char s[] = "hello   ";
        ASSERT_STR_EQ(strtrim(s), "hello", "trailing spaces trimmed");
    }
    PASS();

    TEST("strtrim both sides");
    {
        char s[] = "  hello world  ";
        ASSERT_STR_EQ(strtrim(s), "hello world", "both sides trimmed");
    }
    PASS();

    TEST("strtrim all whitespace");
    {
        char s[] = "   ";
        ASSERT_STR_EQ(strtrim(s), "", "all whitespace becomes empty");
    }
    PASS();

    TEST("strtrim mixed space and tab");
    {
        char s[] = " \t hello \t ";
        ASSERT_STR_EQ(strtrim(s), "hello", "mixed space/tab trimmed");
    }
    PASS();

    TEST("strtrim leading tabs");
    {
        char s[] = "\t\t\thello";
        ASSERT_STR_EQ(strtrim(s), "hello", "leading tabs trimmed");
    }
    PASS();

    TEST("strtrim trailing tabs");
    {
        char s[] = "hello\t\t";
        ASSERT_STR_EQ(strtrim(s), "hello", "trailing tabs trimmed");
    }
    PASS();

    TEST("strtrim returns original pointer");
    {
        char s[] = "  x  ";
        ASSERT(strtrim(s) == s, "returns original pointer");
    }
    PASS();

    TEST("strtrim single leading space");
    {
        char s[] = " hello";
        ASSERT_STR_EQ(strtrim(s), "hello", "one leading space trimmed");
    }
    PASS();

    TEST("strtrim single trailing space");
    {
        char s[] = "hello ";
        ASSERT_STR_EQ(strtrim(s), "hello", "one trailing space trimmed");
    }
    PASS();
}

/* ===================================================================
 *  vsnprintf tests
 * =================================================================== */

static int call_vsnprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

static void test_vsnprintf(void)
{
    printf("\n[vsnprintf]\n");

    TEST("vsnprintf n=0 returns 0 (kernel behavior)");
    {
        char buf[16];
        int r = call_vsnprintf(buf, 0, "hello");
        ASSERT_INT_EQ(r, 0, "n=0 returns 0");
    }
    PASS();

    TEST("vsnprintf n < needed truncates");
    {
        char buf[8];
        memset(buf, 0xAA, sizeof(buf));
        int r = call_vsnprintf(buf, 4, "hello");
        ASSERT_INT_EQ(r, 5, "returned full length (5), not truncated count");
        ASSERT_STR_EQ(buf, "hel", "truncated to 3 chars + null");
    }
    PASS();

    TEST("vsnprintf with sufficient buffer");
    {
        char buf[64];
        int r = call_vsnprintf(buf, sizeof(buf), "hello world");
        ASSERT_INT_EQ(r, 11, "returned length of formatted string");
        ASSERT_STR_EQ(buf, "hello world", "full string written");
    }
    PASS();

    TEST("sprintf return value");
    {
        char buf[64];
        int r = sprintf(buf, "abc%d", 42);
        ASSERT_INT_EQ(r, 5, "sprintf returns length written (abc42)");
        ASSERT_STR_EQ(buf, "abc42", "sprintf output correct");
    }
    PASS();

    TEST("vsnprintf %%s with NULL produces (null)");
    {
        char buf[64];
        int r = call_vsnprintf(buf, sizeof(buf), "%s", NULL);
        ASSERT_INT_EQ(r, 6, "returned length of (null)");
        ASSERT_STR_EQ(buf, "(null)", "NULL string prints as (null)");
    }
    PASS();

    TEST("vsnprintf %%d formats correctly");
    {
        char buf[64];
        int r = call_vsnprintf(buf, sizeof(buf), "%d", 42);
        ASSERT_INT_EQ(r, 2, "length of '42'");
        ASSERT_STR_EQ(buf, "42", "%%d formats correctly");
    }
    PASS();

    TEST("snprintf return value on truncation");
    {
        char buf[8];
        int r = snprintf(buf, 5, "hello world");
        ASSERT_INT_EQ(r, 11, "snprintf returns full formatted length (11)");
        ASSERT_STR_EQ(buf, "hell", "truncated to 4 chars + null");
    }
    PASS();

    TEST("vsnprintf with multiple format args");
    {
        char buf[64];
        int r = call_vsnprintf(buf, sizeof(buf), "%d %s %x",
                               (long long)42, "test", (unsigned long long)0xff);
        ASSERT_INT_EQ(r, 10, "length of '42 test ff'");
        ASSERT_STR_EQ(buf, "42 test ff", "multiple args formatted correctly");
    }
    PASS();

    TEST("vsnprintf NULL buf with n=0");
    {
        int r = call_vsnprintf(NULL, 0, "test");
        ASSERT_INT_EQ(r, 0, "NULL buf with n=0 returns 0");
    }
    PASS();

    TEST("vsnprintf n=1 returns full length (n=0 special-cased)");
    {
        char buf[4] = "xxx";
        int r = call_vsnprintf(buf, 1, "hello");
        ASSERT_INT_EQ(r, 5, "n=1 returns full formatted length (5)");
        ASSERT(buf[0] == '\0', "buf[0] is null terminator");
    }
    PASS();

    TEST("vsnprintf null-terminates after truncation");
    {
        char buf[8];
        memset(buf, 'X', sizeof(buf));
        call_vsnprintf(buf, 5, "abcdefgh");
        ASSERT_INT_EQ(buf[4], '\0', "null-terminated at position 4");
    }
    PASS();

    TEST("vsnprintf with va_list passthrough");
    {
        char buf[64];
        int r = call_vsnprintf(buf, sizeof(buf), "test-%d-%s", 123, "done");
        ASSERT_INT_EQ(r, 13, "length of 'test-123-done'");
        ASSERT_STR_EQ(buf, "test-123-done", "va_list passthrough works");
    }
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
    test_memmove();
    test_memcmp();
    test_memset();
    test_memchr();
    test_strcpy();
    test_strncpy();
    test_strcat();
    test_strncat();
    test_strstr();
    test_strchr();
    test_strrchr();
    test_strtok();
    test_strtok_r();
    test_strsep();
    test_strspn();
    test_strcspn();
    test_strpbrk();
    test_strnlen();
    test_memccpy();
    test_strtrim();

    /* --- stdlib.h tests --- */
    printf("\n--- stdlib.h ---\n");
    test_atoi();
    test_strtol_func();
    test_strtoul_func();
    test_itoa();
    test_ltoa();
    test_strdup_func();
    test_abs();
    test_qsort();
    test_bsearch();
    test_rand();

    /* --- printf tests --- */
    printf("\n--- printf.h ---\n");
    test_sprintf_basic();
    test_sprintf_padding();
    test_sprintf_multiple();
    test_snprintf();
    test_sprintf_advanced();
    test_vsnprintf();

    /* --- string_ext.h tests --- */
    printf("\n--- string_ext.h ---\n");
    test_strlcpy_func();
    test_strlcat_func();
    test_strcasecmp_func();
    test_strncasecmp_func();
    test_strchrnul_func();
    test_strcasestr_func();
    test_strsignal_func();
    test_memmem_func();

    /* --- Summary --- */
    printf("\n=== Results: %d/%d passed, %d failed ===\n",
           tests_passed, tests_run, tests_failed);
    if (tests_failed == 0)
        printf("All tests passed.\n");

    return tests_failed > 0 ? 1 : 0;
}
