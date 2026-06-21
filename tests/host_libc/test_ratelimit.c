/*
 * test_ratelimit.c — Host-side tests for kernel rate limiting
 *
 * Tests __ratelimit from src/kernel/ratelimit.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel types.h + ratelimit.h)
 *  uint64_t comes from <stdint.h>
 * =================================================================== */
struct ratelimit_state {
    uint64_t begin;
    int interval;
    int burst;
    int printed;
};

extern int __ratelimit(struct ratelimit_state *rs);

/* ===================================================================
 *  Stubs for kernel symbols (timer_get_ticks from stubs.o, override with
 *  fake_tick for deterministic testing)
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* Override timer_get_ticks for deterministic testing */
static uint64_t fake_tick = 0;
unsigned long long timer_get_ticks(void) {
    return fake_tick;
}

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
 *  test_ratelimit
 * =================================================================== */
static void test_ratelimit(void)
{
    struct ratelimit_state rs;
    memset(&rs, 0, sizeof(rs));

    /* 1. Initial state: first call returns 1 (allowed) */
    /* interval=0, burst=0 → __ratelimit sets defaults: interval=5, burst=10 */
    fake_tick = 0;
    int r = __ratelimit(&rs);
    TEST("__ratelimit: first call allowed", r == 1);
    TEST("__ratelimit: defaults set", rs.interval == 5 && rs.burst == 10);

    /* 2. Within burst — all allowed */
    for (int i = 1; i < 10; i++) {
        r = __ratelimit(&rs);
        if (r != 1) { TEST("__ratelimit: burst allowed", 0); break; }
    }
    TEST("__ratelimit: burst 10 allowed", rs.printed == 10);

    /* 3. After burst — denied */
    r = __ratelimit(&rs);
    TEST("__ratelimit: after burst denied", r == 0);

    /* 4. Still denied (no time elapsed) */
    r = __ratelimit(&rs);
    TEST("__ratelimit: still denied", r == 0);

    /* 5. Advance time past interval — resets */
    fake_tick += 51;  /* interval * 10 = 50 */
    r = __ratelimit(&rs);
    TEST("__ratelimit: after interval, allowed again", r == 1);

    /* 6. Burst resets after interval */
    TEST("__ratelimit: printed reset after interval", rs.printed == 1);
}

/* ===================================================================
 *  test_ratelimit_custom
 * =================================================================== */
static void test_ratelimit_custom(void)
{
    struct ratelimit_state rs;

    /* Custom interval/burst */
    rs.begin = 0;
    rs.interval = 3;
    rs.burst = 5;
    rs.printed = 0;

    fake_tick = 100;

    /* 1. First call with non-zero interval — allowed */
    int r = __ratelimit(&rs);
    TEST("__ratelimit: custom first allowed", r == 1);

    /* Fill burst */
    for (int i = 1; i < 5; i++) __ratelimit(&rs);

    /* 2. After custom burst — denied */
    r = __ratelimit(&rs);
    TEST("__ratelimit: custom burst denied", r == 0);

    /* 3. Advance time but not enough */
    fake_tick += 20;  /* interval*10 = 30, need 30 from begin=100, now=120 < 130 */
    r = __ratelimit(&rs);
    TEST("__ratelimit: not enough time, denied", r == 0);

    /* 4. Advance enough */
    fake_tick += 20;  /* now=140, begin=100, 140-100=40 >= 30 */
    r = __ratelimit(&rs);
    TEST("__ratelimit: enough time, allowed", r == 1);
}

/* ===================================================================
 *  test_ratelimit_reset
 * =================================================================== */
static void test_ratelimit_reset(void)
{
    struct ratelimit_state rs;
    memset(&rs, 0, sizeof(rs));

    fake_tick = 1000;

    /* Use burst fully */
    for (int i = 0; i < 11; i++) __ratelimit(&rs);
    TEST("__ratelimit: burst exhausted", __ratelimit(&rs) == 0);

    /* Exactly at interval boundary — should reset (>= check) */
    fake_tick += 50;  /* interval=5, so interval*10=50 */
    int r = __ratelimit(&rs);
    TEST("__ratelimit: at exact interval boundary, resets", r == 1);

    /* After reset, state is clean */
    for (int i = 0; i < 9; i++) __ratelimit(&rs);
    TEST("__ratelimit: after reset, burst replenished", __ratelimit(&rs) == 0);
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Rate Limiting Tests ===\n\n");

    printf("--- __ratelimit basic ---\n");
    test_ratelimit();

    printf("\n--- __ratelimit custom ---\n");
    test_ratelimit_custom();

    printf("\n--- __ratelimit reset ---\n");
    test_ratelimit_reset();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
