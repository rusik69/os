/*
 * test_crc16.c — Host-side tests for kernel CRC-16-CCITT
 *
 * Tests crc16, crc16_byte from src/lib/crc16.c.
 * Pure algorithmic — no kernel deps beyond stubs.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel function prototypes (kernel types match system on x86_64)
 * =================================================================== */
extern uint16_t crc16(uint16_t crc, const void *buf, uint32_t len);
extern uint16_t crc16_byte(uint16_t crc, uint8_t byte);

/* ===================================================================
 *  Stubs
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
 *  test_crc16
 * =================================================================== */
static void test_crc16(void)
{
    /* 1. Empty buffer (len=0) — CRC unchanged */
    TEST("crc16: empty buffer keeps seed",
         crc16(0, "anything", 0) == 0);

    /* 2. CRC-16-CCITT variant: polynomial 0x1021, seed=0, no final XOR */
    const char *check_str = "123456789";
    uint16_t c = crc16(0, check_str, strlen(check_str));
    TEST("crc16: check vector '123456789' = 0x31C3",
         c == 0x31C3);

    /* 3. Single byte 'A' */
    uint16_t cA = crc16(0, "A", 1);
    TEST("crc16: single byte non-zero", cA != 0);

    /* 4. Null buffer (len=0) — should not crash */
    uint16_t cnull = crc16(0, NULL, 0);
    TEST("crc16: NULL with len=0 returns seed", cnull == 0);

    /* 5. Chain buffers — cumulative CRC */
    uint16_t c1 = crc16(0, "12", 2);
    uint16_t c2 = crc16(c1, "345", 3);
    uint16_t c3 = crc16(c2, "6789", 4);
    uint16_t c_full = crc16(0, "123456789", 9);
    TEST("crc16: chained updates match single-pass", c3 == c_full);

    /* 6. Non-zero seed */
    uint16_t c_seeded = crc16(0xFFFF, "test", 4);
    TEST("crc16: non-zero seed produces different result",
         c_seeded != crc16(0, "test", 4));

    /* 7. All zeros buffer — CRC(0, zeros, N) = 0 for this implementation */
    char zeros[16];
    memset(zeros, 0, 16);
    uint16_t cz = crc16(0, zeros, 16);
    TEST("crc16: all-zero buffer (semi-property)", cz == 0x0000);

    /* 8. All 0xFF buffer */
    char ff[16];
    memset(ff, 0xFF, 16);
    uint16_t cff = crc16(0, ff, 16);
    TEST("crc16: all-0xFF buffer", cff != 0);

    /* 9. Incremental: byte by byte = single buffer */
    uint16_t inc = 0;
    const char *msg = "CRC16_test";
    for (size_t i = 0; i < strlen(msg); i++)
        inc = crc16(inc, &msg[i], 1);
    uint16_t single = crc16(0, msg, strlen(msg));
    TEST("crc16: byte-at-a-time equals bulk", inc == single);
}

/* ===================================================================
 *  test_crc16_byte
 * =================================================================== */
static void test_crc16_byte(void)
{
    /* 1. Single byte */
    uint16_t cb = crc16_byte(0, 'A');
    TEST("crc16_byte: 'A' produces non-zero", cb != 0);

    /* 2. Cumulative matches crc16 sequence */
    uint16_t cum = 0;
    const char *msg = "Hello!";
    for (size_t i = 0; i < strlen(msg); i++)
        cum = crc16_byte(cum, (uint8_t)msg[i]);
    uint16_t bulk = crc16(0, msg, strlen(msg));
    TEST("crc16_byte: sequential matches crc16", cum == bulk);

    /* 3. Seed propagation */
    uint16_t with_seed = crc16_byte(0xAAAA, 'B');
    TEST("crc16_byte: non-zero seed works",
         with_seed != crc16_byte(0, 'B'));

    /* 4. Each byte affects result (no trivial mapping) */
    uint16_t ca = crc16_byte(0, 'A');
    uint16_t cb2 = crc16_byte(0, 'B');
    TEST("crc16_byte: different bytes different results", ca != cb2);

    /* 5. Zero byte — CRC(0, {0}, 1) = 0 for this impl */
    uint16_t cz = crc16_byte(0, 0);
    TEST("crc16_byte: zero byte", cz == 0x0000);
}

/* ===================================================================
 *  test_crc16_known_vectors
 * =================================================================== */
static void test_crc16_known_vectors(void)
{
    /* Known CRC-16-CCITT values (polynomial 0x1021, seed=0, no final XOR) */
    struct { const char *input; uint16_t expected; } vectors[] = {
        {"",        0x0000},
        {"A",       0x58E5},
        {"123456789", 0x31C3},
        {"Hello",   0xCBD6},
    };

    for (int i = 0; i < 4; i++) {
        uint16_t got = crc16(0, vectors[i].input, strlen(vectors[i].input));
        if (got == vectors[i].expected) {
            printf("  PASS: crc16 known vector \"%s\" = 0x%04X\n",
                   vectors[i].input, (unsigned)got);
            tests_passed++;
        } else {
            printf("  FAIL: crc16 known vector \"%s\" (expected 0x%04X, got 0x%04X)\n",
                   vectors[i].input, (unsigned)vectors[i].expected, (unsigned)got);
            tests_failed++;
        }
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== CRC-16-CCITT Tests ===\n\n");

    printf("--- crc16 ---\n");
    test_crc16();

    printf("\n--- crc16_byte ---\n");
    test_crc16_byte();

    printf("\n--- Known Vectors ---\n");
    test_crc16_known_vectors();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
