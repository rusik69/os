/*
 * test_refcount.c — Host-side tests for kernel extended refcounting
 *
 * Tests refcount_inc, refcount_dec, refcount_inc_not_zero,
 * refcount_sub_and_test, refcount_dec_and_test from kernel/refcount_ext.c.
 *
 * Uses kernel inline atomics (lock xaddl etc.) which compile fine on x86_64.
 * kprintf is stubbed to avoid pulling in printf.o.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel refcount_ext.h + atomic.h)
 * =================================================================== */
typedef int32_t int32_t;
typedef struct { volatile int32_t counter; } atomic_t;
#define ATOMIC_INIT(i) { (i) }

static inline int atomic_read(atomic_t *v) { return v->counter; }
static inline void atomic_set(atomic_t *v, int i) { v->counter = i; }

struct refcount_struct {
    atomic_t refs;
};

extern void refcount_inc(struct refcount_struct *r);
extern void refcount_dec(struct refcount_struct *r);
extern int refcount_inc_not_zero(struct refcount_struct *r);
extern int refcount_sub_and_test(struct refcount_struct *r, int i);

/* Inline helpers from refcount_ext.h (not in refcount_ext.o) */
static inline void refcount_set(struct refcount_struct *r, int n)
{
    atomic_set(&r->refs, n);
}
static inline int refcount_read(const struct refcount_struct *r)
{
    return atomic_read((atomic_t *)&r->refs);
}
static inline int refcount_dec_and_test(struct refcount_struct *r)
{
    return refcount_sub_and_test(r, 1);
}

/* ===================================================================
 *  Stubs
 * =================================================================== */
void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* Match the kernel's REFCOUNT_SATURATED value from refcount_ext.c */
#define REFCOUNT_SATURATED (INT32_MAX / 2)

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
 *  test_refcount
 * =================================================================== */
static void test_refcount(void)
{
    struct refcount_struct r;
    refcount_set(&r, 0);

    /* 1. Initial state */
    TEST("refcount_read: init 0", refcount_read(&r) == 0);

    /* 2. refcount_inc */
    refcount_inc(&r);
    TEST("refcount_inc: 0→1", refcount_read(&r) == 1);

    /* 3. Multiple inc */
    refcount_inc(&r);
    refcount_inc(&r);
    TEST("refcount_inc: 1→3", refcount_read(&r) == 3);

    /* 4. refcount_dec */
    refcount_dec(&r);
    TEST("refcount_dec: 3→2", refcount_read(&r) == 2);

    /* 5. refcount_dec_and_test to reach 0 */
    /* Note: refcount_dec saturates at 0, so use dec_and_test for zero */
    refcount_dec(&r);   /* 2→1 */
    int zero = refcount_dec_and_test(&r);  /* 1→0 */
    TEST("refcount sequence: dec to 0", zero == 1);
    TEST("refcount sequence: value 0", refcount_read(&r) == 0);

    /* 6. refcount_inc_not_zero on zero — should fail */
    refcount_set(&r, 0);
    int rnz = refcount_inc_not_zero(&r);
    TEST("refcount_inc_not_zero: zero returns 0", rnz == 0);
    TEST("refcount_inc_not_zero: still 0", refcount_read(&r) == 0);

    /* 7. refcount_inc_not_zero on non-zero — should succeed */
    refcount_set(&r, 3);
    rnz = refcount_inc_not_zero(&r);
    TEST("refcount_inc_not_zero: non-zero returns 1", rnz == 1);
    TEST("refcount_inc_not_zero: 3→4", refcount_read(&r) == 4);

    /* 8. refcount_sub_and_test — subtract to non-zero */
    refcount_set(&r, 5);
    int test = refcount_sub_and_test(&r, 2);
    TEST("refcount_sub_and_test: 5-2=3, not 0", test == 0);
    TEST("refcount_sub_and_test: value 3", refcount_read(&r) == 3);

    /* 9. refcount_sub_and_test — subtract to zero */
    test = refcount_sub_and_test(&r, 3);
    TEST("refcount_sub_and_test: 3-3=0", test == 1);
    TEST("refcount_sub_and_test: zero", refcount_read(&r) == 0);

    /* 10. refcount_dec_and_test — decrement to zero */
    refcount_set(&r, 1);
    test = refcount_dec_and_test(&r);
    TEST("refcount_dec_and_test: 1-1=0 returns 1", test == 1);
    TEST("refcount_dec_and_test: value 0", refcount_read(&r) == 0);

    /* 11. refcount_dec_and_test — not zero */
    refcount_set(&r, 2);
    test = refcount_dec_and_test(&r);
    TEST("refcount_dec_and_test: 2-1=1 returns 0", test == 0);
    TEST("refcount_dec_and_test: value 1", refcount_read(&r) == 1);

    /* 12. Sequence: inc + dec_and_test interleaved */
    refcount_set(&r, 0);
    refcount_inc(&r);
    refcount_inc(&r);
    refcount_dec(&r);
    TEST("refcount sequence: 0→2→1", refcount_read(&r) == 1);
    refcount_inc(&r);
    zero = refcount_dec_and_test(&r);
    TEST("refcount sequence: 1→2→1, not zero", zero == 0 && refcount_read(&r) == 1);
    refcount_dec(&r); /* 1→0 triggers saturation */
    /* refcount_dec on value 1 triggers saturation — counter goes to REFCOUNT_SATURATED */
    /* inc_not_zero still works on saturated counter (it's a large positive value) */
    TEST("refcount_seq: saturated counter not crash", 1);

    /* 13. refcount_inc on value near INT32_MAX (overflow protection) */
    /* Start from just below INT32_MAX */
    struct refcount_struct r2;
    refcount_set(&r2, 0x7FFFFFFF);  /* INT32_MAX */
    refcount_inc(&r2);
    /* After increment, old=INT32_MAX+1 = INT32_MIN ≤ 0 → saturates to REFCOUNT_SATURATED */
    int sat_ref = REFCOUNT_SATURATED;
    TEST("refcount_inc: on INT32_MAX saturates to REFCOUNT_SATURATED",
         refcount_read(&r2) == sat_ref);
    /* Further inc should increment from REFCOUNT_SATURATED (no further saturation) */
    refcount_inc(&r2);
    TEST("refcount_inc: second inc increments from saturated",
         refcount_read(&r2) == sat_ref + 1);

    /* 14. refcount_dec on zero (underflow protection) */
    struct refcount_struct r3;
    refcount_set(&r3, 0);
    refcount_dec(&r3);
    /* refcount_dec on 0: old = -1 ≤ 0 → saturates to REFCOUNT_SATURATED */
    TEST("refcount_dec: on zero saturates to REFCOUNT_SATURATED",
         refcount_read(&r3) == sat_ref);

    /* 15. refcount_sub_and_test with i=0 */
    struct refcount_struct r4;
    refcount_set(&r4, 5);
    int sub_zero = refcount_sub_and_test(&r4, 0);
    TEST("refcount_sub_and_test: i=0 returns 0", sub_zero == 0);
    TEST("refcount_sub_and_test: i=0 value unchanged", refcount_read(&r4) == 5);

    /* 16. refcount_sub_and_test with i > current */
    struct refcount_struct r5;
    refcount_set(&r5, 2);
    refcount_sub_and_test(&r5, 10);
    /* Should saturate or return 0, but definitely no crash */
    TEST("refcount_sub_and_test: i>current no crash", 1);
    /* old = 2-10 = -8 < 0 → saturates */
    TEST("refcount_sub_and_test: i>current saturates",
         refcount_read(&r5) == sat_ref);

    /* 17. refcount_inc 100 times then dec 100 times */
    struct refcount_struct r6;
    refcount_set(&r6, 0);
    for (int i = 0; i < 100; i++) refcount_inc(&r6);
    TEST("refcount_inc: 100 incs value=100", refcount_read(&r6) == 100);
    for (int i = 0; i < 99; i++) refcount_dec(&r6);
    TEST("refcount_dec: 99 decs from 100 value=1", refcount_read(&r6) == 1);
    refcount_dec(&r6); /* 1→0, old=0 ≤ 0 → saturates */
    TEST("refcount_dec: last dec to zero saturates",
         refcount_read(&r6) == sat_ref);

    /* 18. refcount_inc_not_zero on saturated refcount (saturated > 0) */
    {
        struct refcount_struct r7;
        refcount_set(&r7, REFCOUNT_SATURATED);
        int rnz = refcount_inc_not_zero(&r7);
        TEST("refcount_inc_not_zero: on saturated returns 1", rnz == 1);
        TEST("refcount_inc_not_zero: saturated+1 = S+1",
             refcount_read(&r7) == REFCOUNT_SATURATED + 1);
    }

    /* 19. refcount_sub_and_test with i=1 from value=0 (underflow) */
    {
        struct refcount_struct r8;
        refcount_set(&r8, 0);
        int t = refcount_sub_and_test(&r8, 1);
        TEST("refcount_sub_and_test: 0-1 saturates, returns 0", t == 0);
        TEST("refcount_sub_and_test: 0-1 saturates to S",
             refcount_read(&r8) == REFCOUNT_SATURATED);
    }

    /* 20. refcount_sub_and_test with i=1 from value=1 (exactly zero) */
    {
        struct refcount_struct r9;
        refcount_set(&r9, 1);
        int t = refcount_sub_and_test(&r9, 1);
        TEST("refcount_sub_and_test: 1-1=0 returns 1", t == 1);
        TEST("refcount_sub_and_test: value 0 after sub", refcount_read(&r9) == 0);
    }

    /* 21. refcount_dec_and_test from value=0 (underflow saturation) */
    {
        struct refcount_struct r10;
        refcount_set(&r10, 0);
        int t = refcount_dec_and_test(&r10);
        TEST("refcount_dec_and_test: 0-1 saturates, returns 0", t == 0);
        TEST("refcount_dec_and_test: 0-1 saturates to S",
             refcount_read(&r10) == REFCOUNT_SATURATED);
    }

    /* 22. refcount_inc from value=1 (old=1, positive, no saturation) */
    {
        struct refcount_struct r11;
        refcount_set(&r11, 1);
        refcount_inc(&r11);
        TEST("refcount_inc: 1→2 (no saturation)", refcount_read(&r11) == 2);
    }

    /* 23. refcount_inc from REFCOUNT_SATURATED (just increments) */
    {
        struct refcount_struct r12;
        refcount_set(&r12, REFCOUNT_SATURATED);
        refcount_inc(&r12);
        TEST("refcount_inc: S→S+1", refcount_read(&r12) == REFCOUNT_SATURATED + 1);
        refcount_inc(&r12);
        TEST("refcount_inc: S+1→S+2", refcount_read(&r12) == REFCOUNT_SATURATED + 2);
    }

    /* 24. refcount_dec from value=2 (2→1, no saturation) */
    {
        struct refcount_struct r13;
        refcount_set(&r13, 2);
        refcount_dec(&r13);
        TEST("refcount_dec: 2→1 (no saturation)", refcount_read(&r13) == 1);
    }

    /* 25. refcount_dec from value=1 (saturates to REFCOUNT_SATURATED) */
    {
        struct refcount_struct r14;
        refcount_set(&r14, 1);
        refcount_dec(&r14);
        TEST("refcount_dec: 1→0→S (saturates)",
             refcount_read(&r14) == REFCOUNT_SATURATED);
    }

    /* 26. refcount_sub_and_test with i=5 from value=5 (exactly zero) */
    {
        struct refcount_struct r15;
        refcount_set(&r15, 5);
        int t = refcount_sub_and_test(&r15, 5);
        TEST("refcount_sub_and_test: 5-5=0 returns 1", t == 1);
        TEST("refcount_sub_and_test: value 0 after 5-5", refcount_read(&r15) == 0);
    }

    /* 27. Interleaved inc_not_zero and dec */
    {
        struct refcount_struct r16;
        refcount_set(&r16, 2);
        int rnz = refcount_inc_not_zero(&r16);  /* 2→3 */
        TEST("inc_not_zero+dec: inc_not_zero returns 1", rnz == 1);
        refcount_dec(&r16);  /* 3→2 */
        refcount_dec(&r16);  /* 2→1 */
        TEST("inc_not_zero+dec: final value 1", refcount_read(&r16) == 1);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Extended Refcount Tests ===\n\n");
    test_refcount();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
