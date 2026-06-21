/*
 * test_hweight.c — Host-side tests for kernel Hamming weight (popcount)
 *
 * Tests hweight8, hweight16, hweight32, hweight64 from src/kernel/hweight.c.
 * Pure bit-counting algorithms — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes (kernel types.h: uint8_t = unsigned char etc.)
 * =================================================================== */
extern unsigned int hweight8(uint8_t x);
extern unsigned int hweight16(uint16_t x);
extern unsigned int hweight32(uint32_t x);
extern unsigned int hweight64(uint64_t x);

/* ===================================================================
 *  Reference implementations (software popcount for verification)
 * =================================================================== */
static unsigned int ref_popcount8(uint8_t x) {
    unsigned int c = 0;
    for (int i = 0; i < 8; i++) { if (x & (1U << i)) c++; }
    return c;
}
static unsigned int ref_popcount32(uint32_t x) {
    unsigned int c = 0;
    for (int i = 0; i < 32; i++) { if (x & (1U << i)) c++; }
    return c;
}

/* ===================================================================
 *  Stubs for kernel symbols (kprintf called only by hweight_init — not used)
 * =================================================================== */
void vga_putchar(char c)     { (void)c; }
void serial_putchar(char c)  { (void)c; }
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
 *  test_hweight8
 * =================================================================== */
static void test_hweight8(void)
{
    TEST("hweight8(0) == 0", hweight8(0) == 0);
    TEST("hweight8(0xFF) == 8", hweight8(0xFF) == 8);
    TEST("hweight8(0x55) == 4", hweight8(0x55) == 4);
    TEST("hweight8(0xAA) == 4", hweight8(0xAA) == 4);

    /* Verify all 256 values against reference */
    int all_ok = 1;
    for (int i = 0; i < 256; i++) {
        if (hweight8((uint8_t)i) != ref_popcount8((uint8_t)i)) {
            all_ok = 0;
            break;
        }
    }
    TEST("hweight8: all 256 values match reference", all_ok);

    /* Powers of two */
    TEST("hweight8(1) == 1", hweight8(1) == 1);
    TEST("hweight8(2) == 1", hweight8(2) == 1);
    TEST("hweight8(4) == 1", hweight8(4) == 1);
    TEST("hweight8(128) == 1", hweight8(128) == 1);

    /* Adjacent bits */
    TEST("hweight8(0x03) == 2", hweight8(0x03) == 2);
    TEST("hweight8(0x0F) == 4", hweight8(0x0F) == 4);
    TEST("hweight8(0xF0) == 4", hweight8(0xF0) == 4);

    /* Alternating */
    TEST("hweight8(0xAA) == 4", hweight8(0xAA) == 4);
    TEST("hweight8(0x55) == 4", hweight8(0x55) == 4);
}

/* ===================================================================
 *  test_hweight16
 * =================================================================== */
static void test_hweight16(void)
{
    TEST("hweight16(0) == 0", hweight16(0) == 0);
    TEST("hweight16(0xFFFF) == 16", hweight16(0xFFFF) == 16);
    TEST("hweight16(0x5555) == 8", hweight16(0x5555) == 8);
    TEST("hweight16(0xAAAA) == 8", hweight16(0xAAAA) == 8);
    TEST("hweight16(0xFF00) == 8", hweight16(0xFF00) == 8);
    TEST("hweight16(0x00FF) == 8", hweight16(0x00FF) == 8);
    TEST("hweight16(0x8000) == 1", hweight16(0x8000) == 1);
    TEST("hweight16(0x0001) == 1", hweight16(0x0001) == 1);

    /* Cross-byte boundary */
    TEST("hweight16(0x0101) == 2", hweight16(0x0101) == 2);
    TEST("hweight16(0x8180) == 3", hweight16(0x8180) == 3);

    /* Exhaustive check of first 1024 values */
    int all_ok16 = 1;
    for (int i = 0; i < 1024; i++) {
        unsigned int hw = hweight16((uint16_t)i);
        unsigned int ref = ref_popcount8((uint8_t)(i & 0xFF)) + ref_popcount8((uint8_t)((i >> 8) & 0xFF));
        if (hw != ref) { all_ok16 = 0; break; }
    }
    TEST("hweight16: first 1024 values match reference", all_ok16);

    /* Corner patterns */
    TEST("hweight16(0xFFFF) == 16", hweight16(0xFFFF) == 16);
    TEST("hweight16(0x8000) == 1", hweight16(0x8000) == 1);
    TEST("hweight16(0x0001) == 1", hweight16(0x0001) == 1);
    TEST("hweight16(0x8080) == 2", hweight16(0x8080) == 2);
    TEST("hweight16(0x0101) == 2", hweight16(0x0101) == 2);
}

/* ===================================================================
 *  test_hweight32
 * =================================================================== */
static void test_hweight32(void)
{
    TEST("hweight32(0) == 0", hweight32(0) == 0);
    TEST("hweight32(0xFFFFFFFF) == 32", hweight32(0xFFFFFFFF) == 32);
    TEST("hweight32(0x55555555) == 16", hweight32(0x55555555) == 16);
    TEST("hweight32(0xAAAAAAAA) == 16", hweight32(0xAAAAAAAA) == 16);
    TEST("hweight32(0xFFFF0000) == 16", hweight32(0xFFFF0000) == 16);

    /* Powers of two */
    TEST("hweight32(0x80000000) == 1", hweight32(0x80000000) == 1);
    TEST("hweight32(1U << 15) == 1", hweight32(1U << 15) == 1);
    TEST("hweight32(1U << 0) == 1", hweight32(1) == 1);

    /* Verify against reference */
    uint32_t patterns[] = {
        0x12345678, 0xDEADBEEF, 0x87654321, 0xF0F0F0F0,
        0x0F0F0F0F, 0x11111111, 0xFFFFFFFF, 0x00000000
    };
    int all_ok = 1;
    for (int i = 0; i < 8; i++) {
        if (hweight32(patterns[i]) != ref_popcount32(patterns[i])) {
            all_ok = 0;
            break;
        }
    }
    TEST("hweight32: known patterns match reference", all_ok);

    /* Dense regions */
    TEST("hweight32(0x00000003) == 2", hweight32(0x00000003) == 2);
    TEST("hweight32(0x0000000F) == 4", hweight32(0x0000000F) == 4);
    TEST("hweight32(0x0000FFFF) == 16", hweight32(0x0000FFFF) == 16);
    TEST("hweight32(0x00FFFF00) == 16", hweight32(0x00FFFF00) == 16);

    /* Corner patterns */
    TEST("hweight32(0x80000000) == 1", hweight32(0x80000000U) == 1);
    TEST("hweight32(0xFFFF0000) == 16", hweight32(0xFFFF0000UL) == 16);
    TEST("hweight32(0x0000FFFF) == 16", hweight32(0x0000FFFFUL) == 16);
    TEST("hweight32(0xAAAAAAAA) == 16", hweight32(0xAAAAAAAAUL) == 16);
    TEST("hweight32(0x55555555) == 16", hweight32(0x55555555UL) == 16);
}

/* ===================================================================
 *  test_hweight64
 * =================================================================== */
static void test_hweight64(void)
{
    TEST("hweight64(0) == 0", hweight64(0) == 0);
    TEST("hweight64(0xFFFFFFFFFFFFFFFFULL) == 64",
         hweight64(0xFFFFFFFFFFFFFFFFULL) == 64);
    TEST("hweight64(0x5555555555555555ULL) == 32",
         hweight64(0x5555555555555555ULL) == 32);
    TEST("hweight64(0xAAAAAAAAAAAAAAAAULL) == 32",
         hweight64(0xAAAAAAAAAAAAAAAAULL) == 32);

    /* 64-bit specific: cross-32-bit boundary */
    TEST("hweight64(0x00000001FFFFFFFFULL) == 33",
         hweight64(0x00000001FFFFFFFFULL) == 33);
    TEST("hweight64(0xFFFFFFFF00000000ULL) == 32",
         hweight64(0xFFFFFFFF00000000ULL) == 32);

    /* Single bit at position 63 */
    TEST("hweight64(1ULL << 63) == 1", hweight64(1ULL << 63) == 1);
    TEST("hweight64(1ULL << 31) == 1", hweight64(1ULL << 31) == 1);

    /* Pattern across 64-bit */
    TEST("hweight64(0x1111111111111111ULL) == 16",
         hweight64(0x1111111111111111ULL) == 16);
    TEST("hweight64(0xFFFFFFFF00000000ULL) == 32",
         hweight64(0xFFFFFFFF00000000ULL) == 32);

    /* Alternating patterns */
    TEST("hweight64(0xAAAAAAAAAAAAAAAAULL) == 32",
         hweight64(0xAAAAAAAAAAAAAAAAULL) == 32);
    TEST("hweight64(0x5555555555555555ULL) == 32",
         hweight64(0x5555555555555555ULL) == 32);
    TEST("hweight64(0xF0F0F0F0F0F0F0F0ULL) == 32",
         hweight64(0xF0F0F0F0F0F0F0F0ULL) == 32);
    TEST("hweight64(0x0F0F0F0F0F0F0F0FULL) == 32",
         hweight64(0x0F0F0F0F0F0F0F0FULL) == 32);
    TEST("hweight64(0xFF00FF00FF00FF00ULL) == 32",
         hweight64(0xFF00FF00FF00FF00ULL) == 32);
    TEST("hweight64(0x00FF00FF00FF00FFULL) == 32",
         hweight64(0x00FF00FF00FF00FFULL) == 32);
    TEST("hweight64(0x8000000000000000ULL) == 1",
         hweight64(0x8000000000000000ULL) == 1);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Hamming Weight (Popcount) Tests ===\n\n");

    printf("--- hweight8 ---\n");
    test_hweight8();

    printf("\n--- hweight16 ---\n");
    test_hweight16();

    printf("\n--- hweight32 ---\n");
    test_hweight32();

    printf("\n--- hweight64 ---\n");
    test_hweight64();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
