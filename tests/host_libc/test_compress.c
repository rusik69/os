/*
 * test_compress.c — Host-side tests for kernel LZSS compression.
 *
 * Tests lzss_compress, lzss_decompress from src/kernel/compress.c.
 *
 * Compile: part of host_libc test suite (via Makefile)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ===================================================================
 *  Kernel API declarations
 * =================================================================== */

#define LZSS_WINDOW_SIZE    4096
#define LZSS_MIN_MATCH      3
#define LZSS_MAX_MATCH      18
#define LZSS_HASH_SIZE      4096
#define LZSS_MAX_INPUT      1024
#define LZSS_WORST_CASE(n)  ((n) + (n) / 8 + 16)

extern int lzss_compress(const uint8_t *input, int input_len,
                         uint8_t *output, int output_len);
extern int lzss_decompress(const uint8_t *input, int input_len,
                           uint8_t *output, int output_len);

/* Errno values used by compress.c */
#define EINVAL  22
#define ENOMEM  12
#define ENOSPC  28

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

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
 *  Test helpers
 * =================================================================== */

/* Round-trip: compress then decompress, verify original matches */
static int roundtrip(const uint8_t *orig, int orig_len)
{
    uint8_t comp[LZSS_WORST_CASE(LZSS_MAX_INPUT) + 64];
    uint8_t decomp[LZSS_MAX_INPUT + 64];
    int comp_len, dec_len;

    comp_len = lzss_compress(orig, orig_len, comp, sizeof(comp));
    if (comp_len < 0) return -1;

    dec_len = lzss_decompress(comp, comp_len, decomp, sizeof(decomp));
    if (dec_len != orig_len) return -2;
    if (memcmp(decomp, orig, (size_t)orig_len) != 0) return -3;

    return comp_len;  /* return compressed size */
}

/* ===================================================================
 *  Test: LZSS compress/decompress
 * =================================================================== */

static void test_compress(void)
{
    printf("\n[compress]\n");

    /* 1. Roundtrip "hello world" */
    {
        const uint8_t *msg = (const uint8_t *)"hello world";
        int ret = roundtrip(msg, 11);
        TEST("lzss: 'hello world' roundtrip succeeds", ret >= 0);
    }

    /* 2. Empty buffer (input_len = 0) — should return -EINVAL */
    {
        uint8_t out[128];
        int ret = lzss_compress((const uint8_t *)"", 0, out, sizeof(out));
        TEST("lzss: compress empty buffer returns -EINVAL", ret == -EINVAL);
    }

    /* 3. All-zeros buffer */
    {
        uint8_t zeros[256];
        memset(zeros, 0, sizeof(zeros));
        int ret = roundtrip(zeros, 256);
        TEST("lzss: all-zeros roundtrip succeeds", ret >= 0);
    }

    /* 4. Random-ish buffer */
    {
        uint8_t buf[512];
        for (int i = 0; i < 512; i++) buf[i] = (uint8_t)(i * 37 + 13);
        int ret = roundtrip(buf, 512);
        TEST("lzss: patterned buffer roundtrip succeeds", ret >= 0);
    }

    /* 5. Output too small for compression */
    {
        const uint8_t *msg = (const uint8_t *)"hello world";
        uint8_t tiny[1];
        int ret = lzss_compress(msg, 11, tiny, 1);
        TEST("lzss: output too small returns -ENOMEM",
             ret == -ENOMEM);
    }

    /* 6. Truncated input (decompress with corrupt data) */
    {
        const uint8_t *msg = (const uint8_t *)"AAAAAAA";
        uint8_t comp[LZSS_WORST_CASE(128)];
        uint8_t decomp[128];
        int comp_len = lzss_compress(msg, 7, comp, sizeof(comp));
        if (comp_len > 0) {
            /* Feed only 1 byte of compressed data —
             * should fail because decompressing valid data never
             * yields the original message with just 1 byte of input */
            int ret = lzss_decompress(comp, 1, decomp, sizeof(decomp));
            TEST("lzss: truncated input returns error or 0",
                 ret < 0 || ret == 0);
        }
    }

    /* 7. 1 byte input */
    {
        const uint8_t *one = (const uint8_t *)"X";
        int ret = roundtrip(one, 1);
        TEST("lzss: 1 byte roundtrip succeeds", ret >= 0);
    }

    /* 8. NULL input to compress */
    {
        uint8_t out[128];
        int ret = lzss_compress(NULL, 10, out, sizeof(out));
        TEST("lzss: NULL input returns -EINVAL", ret == -EINVAL);
    }

    /* 9. Large pattern that should compress well (repeating pattern) */
    {
        uint8_t buf[800];
        for (int i = 0; i < 800; i++) buf[i] = (uint8_t)('A' + (i % 10));
        int ret = roundtrip(buf, 800);
        TEST("lzss: repeating pattern roundtrip succeeds", ret >= 0);
        /* It should compress to less than original */
        if (ret >= 0) {
            TEST("lzss: repeating pattern compresses smaller", ret < 800);
        }
    }

    /* 10. Decompress with output buffer too small */
    {
        const uint8_t *msg = (const uint8_t *)"hello world";
        uint8_t comp[LZSS_WORST_CASE(128)];
        uint8_t tiny[2];
        int comp_len = lzss_compress(msg, 11, comp, sizeof(comp));
        if (comp_len > 0) {
            int ret = lzss_decompress(comp, comp_len, tiny, 2);
            TEST("lzss: decompress output too small returns -ENOSPC",
                 ret == -ENOSPC);
        }
    }

    /* 11. Large non-repeating input (edge of max input) */
    {
        uint8_t buf[LZSS_MAX_INPUT];
        for (int i = 0; i < LZSS_MAX_INPUT; i++) buf[i] = (uint8_t)(i & 0xFF);
        int ret = roundtrip(buf, LZSS_MAX_INPUT);
        TEST("lzss: max input roundtrip succeeds", ret >= 0);
    }

    /* 12. Input exceeds LZSS_MAX_INPUT */
    {
        uint8_t big[LZSS_MAX_INPUT + 1];
        uint8_t out[LZSS_WORST_CASE(LZSS_MAX_INPUT) + 64];
        memset(big, 'A', sizeof(big));
        int ret = lzss_compress(big, LZSS_MAX_INPUT + 1, out, sizeof(out));
        TEST("lzss: input exceeds max returns -EINVAL", ret == -EINVAL);
    }

    /* 13. Very small input (2 bytes "AB") */
    {
        const uint8_t *ab = (const uint8_t *)"AB";
        int ret = roundtrip(ab, 2);
        TEST("lzss: 2-byte roundtrip succeeds", ret >= 0);
    }

    /* 14. Highly repetitive input (all 'A's 64 bytes) */
    {
        uint8_t aaaa[64];
        memset(aaaa, 'A', 64);
        int ret = roundtrip(aaaa, 64);
        TEST("lzss: 64 'A's roundtrip succeeds", ret >= 0);
        if (ret >= 0) {
            TEST("lzss: 64 'A's compresses smaller", ret < 64);
        }
    }

    /* 15. Multiple alternating roundtrips */
    {
        const uint8_t *msg1 = (const uint8_t *)"FirstMessage";
        const uint8_t *msg2 = (const uint8_t *)"SecondMessage";
        int r1 = roundtrip(msg1, 12);
        int r2 = roundtrip(msg2, 13);
        TEST("lzss: alternating roundtrip 1", r1 >= 0);
        TEST("lzss: alternating roundtrip 2", r2 >= 0);
    }

    /* 16. Compress exactly 3 bytes (LZSS_MIN_MATCH boundary) */
    {
        uint8_t three[3] = { 'A', 'B', 'C' };
        int ret = roundtrip(three, 3);
        TEST("lzss: 3-byte (min match) roundtrip succeeds", ret >= 0);
    }

    /* 17. Compress NULL output returns -EINVAL */
    {
        const uint8_t *msg = (const uint8_t *)"test";
        int ret = lzss_compress(msg, 4, NULL, 100);
        TEST("lzss: compress NULL output returns -EINVAL", ret == -EINVAL);
    }

    /* 18. Input of exactly 4 bytes (LZSS_MIN_MATCH+1) */
    {
        const uint8_t *four = (const uint8_t *)"ABCD";
        int ret = roundtrip(four, 4);
        TEST("lzss: 4-byte roundtrip succeeds", ret >= 0);
    }

    /* 19. Input of exactly 18 bytes (LZSS_MAX_MATCH) */
    {
        const uint8_t *eighteen = (const uint8_t *)"ABCDEFGHIJKLMNOPQR";
        int ret = roundtrip(eighteen, 18);
        TEST("lzss: 18-byte (max match) roundtrip succeeds", ret >= 0);
    }

    /* 20. Input of 19 bytes (LZSS_MAX_MATCH+1) */
    {
        const uint8_t *nineteen = (const uint8_t *)"ABCDEFGHIJKLMNOPQRS";
        int ret = roundtrip(nineteen, 19);
        TEST("lzss: 19-byte roundtrip succeeds", ret >= 0);
    }

    /* 21. Wide character range (0x00-0xFF) */
    {
        uint8_t wide[256];
        for (int i = 0; i < 256; i++) wide[i] = (uint8_t)i;
        int ret = roundtrip(wide, 256);
        TEST("lzss: 0x00-0xFF roundtrip succeeds", ret >= 0);
    }

    /* 22. Compress and decompress: deterministic output */
    {
        uint8_t data[32];
        memset(data, 'X', 32);
        uint8_t comp1[LZSS_WORST_CASE(128)], comp2[LZSS_WORST_CASE(128)];
        int len1 = lzss_compress(data, 32, comp1, sizeof(comp1));
        int len2 = lzss_compress(data, 32, comp2, sizeof(comp2));
        TEST("lzss: deterministic compression size", len1 == len2);
        if (len1 == len2 && len1 > 0) {
            TEST("lzss: deterministic compression content",
                 memcmp(comp1, comp2, len1) == 0);
        }
    }

    /* 23. Decompress with NULL input */
    {
        uint8_t out[64];
        int ret = lzss_decompress(NULL, 10, out, sizeof(out));
        TEST("lzss: decompress NULL input returns -EINVAL", ret == -EINVAL);
    }

    /* 24. Decompress with NULL output */
    {
        uint8_t in[64];
        memset(in, 0, 64);
        int ret = lzss_decompress(in, 10, NULL, 100);
        TEST("lzss: decompress NULL output returns -EINVAL", ret == -EINVAL);
    }

    /* 25. Compress with 0 input length (but non-NULL data) */
    {
        uint8_t out[128];
        int ret = lzss_compress((const uint8_t *)"data", 0, out, sizeof(out));
        TEST("lzss: compress zero length returns -EINVAL", ret == -EINVAL);
    }

    /* 26. Decompress with zero input length */
    {
        uint8_t out[64];
        int ret = lzss_decompress((const uint8_t *)"", 0, out, sizeof(out));
        TEST("lzss: decompress zero input returns -EINVAL", ret == -EINVAL);
    }

    /* 27. 2 identical bytes repeated — well-compressible pattern */
    {
        uint8_t pattern[64];
        for (int i = 0; i < 64; i++) pattern[i] = (uint8_t)((i % 2) ? 0x55 : 0xAA);
        int ret = roundtrip(pattern, 64);
        TEST("lzss: alternating pattern roundtrip succeeds", ret >= 0);
        if (ret >= 0) {
            TEST("lzss: alternating pattern compresses", ret < 64);
        }
    }

    /* 28. Ramp-up: increasing repeating pattern length to trigger matches */
    {
        uint8_t ramp[800];
        for (int i = 0; i < 800; i++) ramp[i] = (uint8_t)('A' + (i % 5));
        int ret = roundtrip(ramp, 800);
        TEST("lzss: ramp 5-char pattern roundtrip succeeds", ret >= 0);
    }

    /* 29. Minimum compressible: 3 identical bytes (LZSS_MIN_MATCH) should compress */
    {
        uint8_t three[3] = { 'X', 'X', 'X' };
        int ret = roundtrip(three, 3);
        TEST("lzss: 3 identical bytes roundtrip succeeds", ret >= 0);
    }

    /* 30. 2 identical bytes (below LZSS_MIN_MATCH) */
    {
        uint8_t two[2] = { 'X', 'X' };
        int ret = roundtrip(two, 2);
        TEST("lzss: 2 identical bytes roundtrip succeeds", ret >= 0);
    }

    /* 31. Compress with output_len exactly matching worst-case size */
    {
        const uint8_t *msg = (const uint8_t *)"compression test";
        uint8_t comp[LZSS_WORST_CASE(32)];
        int ret = lzss_compress(msg, 17, comp, sizeof(comp));
        TEST("lzss: compress with exact sized output buffer", ret >= 0 || ret == -ENOMEM);
    }

    /* 32. Multiple roundtrips with same data (deterministic compressed size) */
    {
        uint8_t data[64];
        for (int i = 0; i < 64; i++) data[i] = (uint8_t)(i * 3 + 7);
        int sizes[3];
        for (int i = 0; i < 3; i++) {
            sizes[i] = roundtrip(data, 64);
            if (sizes[i] < 0) { TEST("lzss: deterministic roundtrip", 0); break; }
        }
        TEST("lzss: multiple roundtrips consistent",
             sizes[0] >= 0 && sizes[0] == sizes[1] && sizes[1] == sizes[2]);
    }

    /* 33. Decompress with exact output length matching original */
    {
        uint8_t data[32];
        for (int i = 0; i < 32; i++) data[i] = (uint8_t)(i * 7 + 11);
        uint8_t comp[LZSS_WORST_CASE(64)];
        uint8_t decomp[32];
        int comp_len = lzss_compress(data, 32, comp, sizeof(comp));
        if (comp_len > 0) {
            int dec_len = lzss_decompress(comp, comp_len, decomp, 32);
            TEST("lzss: exact decompress size matches", dec_len == 32);
            if (dec_len == 32) {
                TEST("lzss: exact decompress content matches",
                     memcmp(decomp, data, 32) == 0);
            }
        }
    }

    /* 34. Compress large non-repeating input below max */
    {
        uint8_t buf[512];
        for (int i = 0; i < 512; i++) buf[i] = (uint8_t)((i * 157 + 11) & 0xFF);
        int ret = roundtrip(buf, 512);
        TEST("lzss: 512-byte random roundtrip succeeds", ret >= 0);
    }

    /* 35. Compress with input of varying lengths */
    {
        uint8_t data[128];
        for (int i = 0; i < 128; i++) data[i] = (uint8_t)(i * 5 + 3);
        int ret = roundtrip(data, 128);
        TEST("lzss: 128-byte roundtrip succeeds", ret >= 0);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Kernel LZSS Compression Tests ===\n");
    test_compress();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
