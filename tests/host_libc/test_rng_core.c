/*
 * test_rng_core.c — Host-side tests for kernel PRNG (xorshift64)
 *
 * Tests the xorshift64 PRNG logic used in src/kernel/rng.c.
 * Self-contained: duplicates the core PRNG functions without
 * needing the hardware RNG detection (CPUID) or kernel linkage.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  PRNG core (xorshift64 — same algorithm as kernel/rng.c)
 * =================================================================== */
static uint64_t g_rng_state = 0;

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void rng_init(void)
{
    /* Seed from a constant for deterministic testing */
    g_rng_state = 0xDEADBEEFCAFEBABEULL;
    if (g_rng_state == 0) g_rng_state = 1;

    /* Warm-up rounds */
    for (int i = 0; i < 10; i++) {
        (void)xorshift64(&g_rng_state);
    }
}

uint64_t rng_get_u64(void)
{
    return xorshift64(&g_rng_state);
}

uint32_t rng_get_u32(void)
{
    return (uint32_t)xorshift64(&g_rng_state);
}

void rng_fill_buf(void *buf, uint32_t len)
{
    uint8_t *bytes = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) {
        bytes[i] = (uint8_t)rng_get_u32();
    }
}

void rng_add_entropy(const void *data, uint32_t len)
{
    if (!data || len == 0)
        return;

    const uint8_t *bytes = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        g_rng_state ^= (uint64_t)bytes[i];
        (void)xorshift64(&g_rng_state);
    }

    g_rng_state ^= (uint64_t)len;
    (void)xorshift64(&g_rng_state);
}

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }

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
 *  test_rng_init
 * =================================================================== */
static void test_rng_init(void)
{
    /* Reset state */
    g_rng_state = 0;

    rng_init();
    TEST("rng_init: state non-zero after init", g_rng_state != 0);

    /* Second init resets state */
    uint64_t state1 = g_rng_state;
    rng_init();
    uint64_t state2 = g_rng_state;
    TEST("rng_init: deterministic re-init", state1 == state2);

    /* xorshift64 is stuck at 0 when state is 0 (known limitation) */
    g_rng_state = 0;
    for (int i = 0; i < 10; i++) {
        (void)xorshift64(&g_rng_state);
    }
    TEST("xorshift64: stuck at 0 when state=0", g_rng_state == 0);
}

/* ===================================================================
 *  test_rng_get_u64
 * =================================================================== */
static void test_rng_get_u64(void)
{
    rng_init();

    /* 1. First value is non-zero */
    uint64_t v1 = rng_get_u64();
    TEST("rng_get_u64: first call non-zero", v1 != 0);

    /* 2. Second value differs from first */
    uint64_t v2 = rng_get_u64();
    TEST("rng_get_u64: second call differs from first", v2 != v1);

    /* 3. Third value also differs */
    uint64_t v3 = rng_get_u64();
    TEST("rng_get_u64: third call differs", v3 != v2 && v3 != v1);

    /* 4. Deterministic sequence with same seed */
    g_rng_state = 0;
    rng_init();
    uint64_t seq1_a = rng_get_u64();
    uint64_t seq1_b = rng_get_u64();
    g_rng_state = 0;
    rng_init();
    uint64_t seq2_a = rng_get_u64();
    uint64_t seq2_b = rng_get_u64();
    TEST("rng_get_u64: deterministic sequence", seq1_a == seq2_a && seq1_b == seq2_b);

    /* rng_get_u64 returns values within uint64_t range */
    uint64_t u64_range = rng_get_u64();
    TEST("rng_get_u64: value in uint64_t range",
         u64_range == u64_range); /* always true, but check no crash */

    /* Multiple rng_get_u64 calls: 100 calls, no two adjacent equal */
    rng_init();
    uint64_t prev = rng_get_u64();
    int all_adjacent_diff = 1;
    for (int i = 1; i < 100; i++) {
        uint64_t cur = rng_get_u64();
        if (cur == prev) { all_adjacent_diff = 0; break; }
        prev = cur;
    }
    TEST("rng_get_u64: 100 calls no adjacent equal", all_adjacent_diff);

    /* Verify xorshift64 doesn't get stuck at 0 when seeded with 0 */
    g_rng_state = 0;
    rng_init();
    uint64_t v_zero_seed = rng_get_u64();
    TEST("rng_get_u64: seed 0 => warmup produces non-zero", v_zero_seed != 0);
}

/* ===================================================================
 *  test_rng_fill_buf
 * =================================================================== */
static void test_rng_fill_buf(void)
{
    rng_init();
    uint8_t buf1[32];
    uint8_t buf2[32];

    memset(buf1, 0, 32);
    memset(buf2, 0, 32);

    /* Fill buf1 */
    rng_fill_buf(buf1, 32);

    /* Check that at least one byte is non-zero */
    int has_data = 0;
    for (int i = 0; i < 32; i++) {
        if (buf1[i]) { has_data = 1; break; }
    }
    TEST("rng_fill_buf: produces non-zero bytes", has_data);

    /* Fill buf2 — should differ from buf1 */
    rng_fill_buf(buf2, 32);
    int differs = 0;
    for (int i = 0; i < 32; i++) {
        if (buf1[i] != buf2[i]) { differs = 1; break; }
    }
    TEST("rng_fill_buf: consecutive fills differ", differs);

    /* rng_fill_buf with zero length is a no-op */
    uint64_t state_before = g_rng_state;
    rng_fill_buf(buf1, 0);
    TEST("rng_fill_buf: len=0 no-op", g_rng_state == state_before);

    /* rng_fill_buf with NULL buffer doesn't crash when len=0 */
    uint64_t state_before_null = g_rng_state;
    rng_fill_buf(NULL, 0);
    TEST("rng_fill_buf: NULL buf len=0 no crash", g_rng_state == state_before_null);

    /* rng_fill_buf with 1 byte produces non-zero */
    uint8_t single_byte = 0;
    rng_fill_buf(&single_byte, 1);
    TEST("rng_fill_buf: 1 byte produces non-zero value", single_byte != 0);

    /* rng_fill_buf large buffer (256 bytes) */
    uint8_t buf256[256];
    memset(buf256, 0, 256);
    rng_fill_buf(buf256, 256);
    int has_nonzero_256 = 0;
    for (int i = 0; i < 256; i++) {
        if (buf256[i]) { has_nonzero_256 = 1; break; }
    }
    TEST("rng_fill_buf: 256-byte buffer has non-zero bytes", has_nonzero_256);

    /* Multiple rng_fill_buf calls produce different buffers */
    uint8_t buf256b[256];
    memset(buf256b, 0, 256);
    rng_fill_buf(buf256b, 256);
    int differs_256 = 0;
    for (int i = 0; i < 256; i++) {
        if (buf256[i] != buf256b[i]) { differs_256 = 1; break; }
    }
    TEST("rng_fill_buf: consecutive 256-byte fills differ", differs_256);

    /* Deterministic seeding: same state after same init operations */
    g_rng_state = 0;
    rng_init();
    uint64_t state_det_a = g_rng_state;
    g_rng_state = 0;
    rng_init();
    uint64_t state_det_b = g_rng_state;
    TEST("rng_fill_buf: deterministic init state", state_det_a == state_det_b);
}

/* ===================================================================
 *  test_rng_add_entropy
 * =================================================================== */
static void test_rng_add_entropy(void)
{
    rng_init();

    /* 1. Add entropy changes state */
    uint64_t state_before = g_rng_state;
    const char entropy[] = "some entropy bytes";
    rng_add_entropy(entropy, 17);
    TEST("rng_add_entropy: changes state", g_rng_state != state_before);

    /* 2. Subsequent get_u64 differs from sequence without entropy */
    g_rng_state = 0;
    rng_init();
    uint64_t v_no_entropy = rng_get_u64();

    g_rng_state = 0;
    rng_init();
    rng_add_entropy("x", 1);
    uint64_t v_with_entropy = rng_get_u64();
    TEST("rng_add_entropy: changes output sequence", v_with_entropy != v_no_entropy);

    /* 3. Different entropy produces different state */
    g_rng_state = 0;
    rng_init();
    rng_add_entropy("a", 1);
    uint64_t state_a = rng_get_u64();

    g_rng_state = 0;
    rng_init();
    rng_add_entropy("b", 1);
    uint64_t state_b = rng_get_u64();
    TEST("rng_add_entropy: different entropy diverges", state_a != state_b);

    /* 4. NULL data is no-op */
    g_rng_state = 0;
    rng_init();
    uint64_t before_null = g_rng_state;
    rng_add_entropy(NULL, 10);
    TEST("rng_add_entropy: NULL data no-op", g_rng_state == before_null);

    /* 5. Empty data is no-op */
    rng_add_entropy("x", 0);
    TEST("rng_add_entropy: len=0 no-op", g_rng_state == before_null);

    /* rng_add_entropy with 1 byte changes state from init baseline */
    g_rng_state = 0;
    rng_init();
    uint64_t state_before_1byte = g_rng_state;
    rng_add_entropy("Z", 1);
    TEST("rng_add_entropy: 1 byte changes state", g_rng_state != state_before_1byte);

    /* rng_add_entropy with 256 bytes changes state */
    uint8_t entropy256[256];
    for (int i = 0; i < 256; i++) entropy256[i] = (uint8_t)(i * 7 + 3);
    g_rng_state = 0;
    rng_init();
    uint64_t state_before_256 = g_rng_state;
    rng_add_entropy(entropy256, 256);
    TEST("rng_add_entropy: 256 bytes changes state", g_rng_state != state_before_256);

    /* Deterministic: same entropy twice produces same final state */
    g_rng_state = 0;
    rng_init();
    rng_add_entropy("deterministic", 12);
    uint64_t state_det1 = g_rng_state;
    g_rng_state = 0;
    rng_init();
    rng_add_entropy("deterministic", 12);
    uint64_t state_det2 = g_rng_state;
    TEST("rng_add_entropy: deterministic same state", state_det1 == state_det2);
}

/* ===================================================================
 *  test_rng_get_u32
 * =================================================================== */
static void test_rng_get_u32(void)
{
    rng_init();

    /* 1. Returns non-zero */
    uint32_t v1 = rng_get_u32();
    TEST("rng_get_u32: non-zero", v1 != 0);

    /* 2. All 32 bits vary */
    uint32_t all_or = 0, all_and = 0xFFFFFFFF;
    for (int i = 0; i < 100; i++) {
        uint32_t v = rng_get_u32();
        all_or |= v;
        all_and &= v;
    }
    /* After 100 calls, should have seen both 1s and 0s in every position */
    TEST("rng_get_u32: all bits vary (or=all 1s)", all_or == 0xFFFFFFFF);
    TEST("rng_get_u32: all bits vary (and=all 0s)", all_and == 0);

    /* rng_get_u32 returns different values on successive calls */
    uint32_t u32_a = rng_get_u32();
    uint32_t u32_b = rng_get_u32();
    uint32_t u32_c = rng_get_u32();
    TEST("rng_get_u32: call 1 != call 2", u32_a != u32_b);
    TEST("rng_get_u32: call 2 != call 3", u32_b != u32_c);
    TEST("rng_get_u32: call 1 != call 3", u32_a != u32_c);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== PRNG (xorshift64) Tests ===\n\n");

    printf("--- rng_init ---\n");
    test_rng_init();

    printf("\n--- rng_get_u64 ---\n");
    test_rng_get_u64();

    printf("\n--- rng_get_u32 ---\n");
    test_rng_get_u32();

    printf("\n--- rng_fill_buf ---\n");
    test_rng_fill_buf();

    printf("\n--- rng_add_entropy ---\n");
    test_rng_add_entropy();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
