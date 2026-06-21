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
