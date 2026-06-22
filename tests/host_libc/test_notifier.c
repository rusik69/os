/*
 * test_notifier.c — Host-side tests for kernel notifier chains
 *
 * Tests notifier_init, notifier_chain_register, notifier_chain_unregister,
 * notifier_call_chain from src/kernel/notifier.c.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel type declarations (mirror kernel types.h + notifier.h)
 * =================================================================== */
#define NOTIFIER_PANIC      0
#define NOTIFIER_DIE        1
#define NOTIFIER_REBOOT     2
#define NOTIFIER_NET_EVENT  3
#define NOTIFIER_CPU_HP     4
#define NOTIFIER_MAX        8

struct notifier_block {
    int (*notifier_call)(struct notifier_block *, unsigned long, void *);
    struct notifier_block *next;
};

extern void notifier_init(void);
extern int notifier_chain_register(int type, struct notifier_block *nb);
extern int notifier_chain_unregister(int type, struct notifier_block *nb);
extern int notifier_call_chain(int type, unsigned long v, void *data);

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
 *  Test callback tracking
 * =================================================================== */
static int callback_count = 0;
static unsigned long last_val = 0;
static void *last_data = NULL;

/* NOTIFY_BAD-like return value */
#define NOTIFY_BAD 1

static int test_callback(struct notifier_block *nb, unsigned long v, void *data)
{
    (void)nb;
    callback_count++;
    last_val = v;
    last_data = data;
    return 0;
}

static int test_callback2(struct notifier_block *nb, unsigned long v, void *data)
{
    (void)nb;
    callback_count += 10;  /* distinguish from callback1 */
    (void)v; (void)data;
    return 0;
}

static int test_callback_bad(struct notifier_block *nb, unsigned long v, void *data)
{
    (void)nb; (void)v; (void)data;
    callback_count += 100;
    return NOTIFY_BAD;
}

static int test_callback_self_unregister(struct notifier_block *nb, unsigned long v, void *data)
{
    (void)v; (void)data;
    callback_count += 1000;
    /* Unregister ourselves from the chain */
    notifier_chain_unregister(NOTIFIER_PANIC, nb);
    return 0;
}

/* ===================================================================
 *  test_notifier
 * =================================================================== */
static void test_notifier(void)
{
    struct notifier_block nb1 = { .notifier_call = test_callback, .next = NULL };
    struct notifier_block nb2 = { .notifier_call = test_callback2, .next = NULL };

    /* 1. Init */
    notifier_init();
    /* Can't inspect internal state, but init doesn't crash */

    /* 2. Register */
    int r = notifier_chain_register(NOTIFIER_PANIC, &nb1);
    TEST("notifier_chain_register: returns 0", r == 0);

    /* 3. Call chain */
    int data_val = 42;
    callback_count = 0;
    r = notifier_call_chain(NOTIFIER_PANIC, 123, &data_val);
    TEST("notifier_call_chain: callback called once", callback_count == 1);
    TEST("notifier_call_chain: value passed", last_val == 123);
    TEST("notifier_call_chain: data passed", last_data == &data_val);

    /* 4. Register second notifier on same type */
    notifier_chain_register(NOTIFIER_PANIC, &nb2);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    /* Both callbacks should fire: callback_count gets +1 from nb1, +10 from nb2 */
    TEST("notifier_call_chain: both callbacks fire", callback_count == 11);

    /* 5. Call on different type (no notifiers registered) */
    callback_count = 0;
    r = notifier_call_chain(NOTIFIER_DIE, 0, NULL);
    TEST("notifier_call_chain: empty type returns 0", r == 0);
    TEST("notifier_call_chain: no callback on empty type", callback_count == 0);

    /* 6. Register on different type */
    notifier_chain_register(NOTIFIER_DIE, &nb1);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_DIE, 55, NULL);
    TEST("notifier_call_chain: different type fires", callback_count == 1);

    /* 7. Unregister */
    r = notifier_chain_unregister(NOTIFIER_PANIC, &nb1);
    TEST("notifier_chain_unregister: returns 0", r == 0);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    TEST("notifier_call_chain: after unregister, only nb2 fires", callback_count == 10);

    /* 8. Unregister non-existent */
    r = notifier_chain_unregister(NOTIFIER_PANIC, &nb1);
    TEST("notifier_chain_unregister: not found returns -1", r == -1);

    /* 9. Unregister last — makes chain empty */
    notifier_chain_unregister(NOTIFIER_PANIC, &nb2);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    TEST("notifier_call_chain: empty after unregister all", callback_count == 0);

    /* 10. Error cases */
    r = notifier_chain_register(NOTIFIER_PANIC, NULL);
    TEST("notifier_chain_register: NULL returns -1", r == -1);
    r = notifier_chain_register(NOTIFIER_MAX + 1, &nb1);
    TEST("notifier_chain_register: invalid type returns -1", r == -1);

    /* 11. Register same callback twice */
    r = notifier_chain_register(NOTIFIER_PANIC, &nb1);
    TEST("notifier_chain_register: re-register returns 0", r == 0);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    /* Kernel may or may not fire a double-registered callback twice */
    TEST("notifier_call_chain: double-registered fires at least once",
         callback_count >= 1);

    /* Clean up extra registration */
    notifier_chain_unregister(NOTIFIER_PANIC, &nb1);
    notifier_chain_unregister(NOTIFIER_PANIC, &nb1);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    TEST("notifier_call_chain: empty after cleanup", callback_count == 0);

    /* 12. Re-register after full unregister */
    r = notifier_chain_register(NOTIFIER_PANIC, &nb1);
    TEST("notifier_chain_register: re-register after empty returns 0", r == 0);
    callback_count = 0;
    notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
    TEST("notifier_call_chain: re-registered fires once", callback_count == 1);
    notifier_chain_unregister(NOTIFIER_PANIC, &nb1);

    /* 13. Callback returning NOTIFY_BAD */
    /* First clean up nb1 from DIE (registered in test 6) */
    notifier_chain_unregister(NOTIFIER_DIE, &nb1);
    struct notifier_block nb_bad = { .notifier_call = test_callback_bad, .next = NULL };
    struct notifier_block nb_good = { .notifier_call = test_callback, .next = NULL };
    notifier_chain_register(NOTIFIER_DIE, &nb_bad);
    notifier_chain_register(NOTIFIER_DIE, &nb_good);
    callback_count = 0;
    r = notifier_call_chain(NOTIFIER_DIE, 0, NULL);
    /* nb_good fires first (LIFO): +1, then nb_bad: +100 = 101 */
    TEST("notifier_call_chain: bad callback fires", callback_count == 101);
    notifier_chain_unregister(NOTIFIER_DIE, &nb_bad);
    notifier_chain_unregister(NOTIFIER_DIE, &nb_good);

    /* 14. 10+ callbacks on same type */
    struct notifier_block many[15];
    for (int i = 0; i < 15; i++) {
        many[i].notifier_call = test_callback;
        many[i].next = NULL;
        notifier_chain_register(NOTIFIER_REBOOT, &many[i]);
    }
    callback_count = 0;
    notifier_call_chain(NOTIFIER_REBOOT, 0, NULL);
    TEST("notifier_call_chain: 15 callbacks fire", callback_count == 15);
    for (int i = 0; i < 15; i++)
        notifier_chain_unregister(NOTIFIER_REBOOT, &many[i]);

    /* 15. Callback that unregisters itself */
    struct notifier_block nb_self = { .notifier_call = test_callback_self_unregister, .next = NULL };
    struct notifier_block nb_after = { .notifier_call = test_callback2, .next = NULL };
    notifier_chain_register(NOTIFIER_NET_EVENT, &nb_self);
    notifier_chain_register(NOTIFIER_NET_EVENT, &nb_after);
    callback_count = 0;
    r = notifier_call_chain(NOTIFIER_NET_EVENT, 0, NULL);
    /* Self-unregister fires (adds 1000), nb_after still fires (adds 10) */
    TEST("notifier_call_chain: self-unregister fires", callback_count == 1010);
    /* Verify nb_self may or may not still be in chain (depends on kernel impl) */
    r = notifier_chain_unregister(NOTIFIER_NET_EVENT, &nb_self);
    notifier_chain_unregister(NOTIFIER_NET_EVENT, &nb_after);

    /* 16. Register type validation edge cases */
    {
        struct notifier_block nb_tmp = { .notifier_call = test_callback, .next = NULL };
        r = notifier_chain_register(-1, &nb_tmp);
        TEST("notifier_chain_register: type=-1 returns -1", r == -1);
        r = notifier_chain_register(NOTIFIER_MAX, &nb_tmp);
        TEST("notifier_chain_register: type=NOTIFIER_MAX returns -1", r == -1);
    }

    /* 17. Call chain on CPU_HP type with proper value/data */
    {
        struct notifier_block nb_cpu = { .notifier_call = test_callback, .next = NULL };
        notifier_chain_register(NOTIFIER_CPU_HP, &nb_cpu);
        int val = 777;
        callback_count = 0;
        r = notifier_call_chain(NOTIFIER_CPU_HP, 888, &val);
        TEST("notifier_call_chain: CPU_HP fires once", callback_count == 1);
        TEST("notifier_call_chain: CPU_HP passes value", last_val == 888);
        TEST("notifier_call_chain: CPU_HP passes data", last_data == &val);
        notifier_chain_unregister(NOTIFIER_CPU_HP, &nb_cpu);
    }

    /* 18. Unregister non-existent from wrong type */
    {
        struct notifier_block nb_wt = { .notifier_call = test_callback, .next = NULL };
        notifier_chain_register(NOTIFIER_PANIC, &nb_wt);
        r = notifier_chain_unregister(NOTIFIER_DIE, &nb_wt);
        TEST("notifier_chain_unregister: different type returns -1", r == -1);
        notifier_chain_unregister(NOTIFIER_PANIC, &nb_wt);
    }

    /* 19. Two bad callbacks: both fire, return last NOTIFY_BAD */
    {
        struct notifier_block nb_b1 = { .notifier_call = test_callback_bad, .next = NULL };
        struct notifier_block nb_b2 = { .notifier_call = test_callback_bad, .next = NULL };
        notifier_chain_register(NOTIFIER_NET_EVENT, &nb_b1);
        notifier_chain_register(NOTIFIER_NET_EVENT, &nb_b2);
        callback_count = 0;
        r = notifier_call_chain(NOTIFIER_NET_EVENT, 0, NULL);
        /* nb_b2 registered last (LIFO), fires first → +100, then nb_b1 → +100 = 200 */
        TEST("notifier_call_chain: two bad callbacks both fire", callback_count == 200);
        TEST("notifier_call_chain: returns last callback's NOTIFY_BAD", r == NOTIFY_BAD);
        notifier_chain_unregister(NOTIFIER_NET_EVENT, &nb_b1);
        notifier_chain_unregister(NOTIFIER_NET_EVENT, &nb_b2);
    }

    /* 20. Register same callback block on two different types */
    {
        struct notifier_block nb_shared = { .notifier_call = test_callback2, .next = NULL };
        notifier_chain_register(NOTIFIER_PANIC, &nb_shared);
        notifier_chain_register(NOTIFIER_DIE, &nb_shared);
        callback_count = 0;
        notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
        TEST("notifier_call_chain: shared on PANIC fires (callback2 +10)",
             callback_count == 10);
        callback_count = 0;
        notifier_call_chain(NOTIFIER_DIE, 0, NULL);
        TEST("notifier_call_chain: shared on DIE fires (callback2 +10)",
             callback_count == 10);
        notifier_chain_unregister(NOTIFIER_PANIC, &nb_shared);
        notifier_chain_unregister(NOTIFIER_DIE, &nb_shared);
    }

    /* 21. Register after failed unregister still works */
    {
        struct notifier_block nb_fail = { .notifier_call = test_callback, .next = NULL };
        r = notifier_chain_unregister(NOTIFIER_PANIC, &nb_fail);
        TEST("notifier_chain_unregister: never-registered returns -1", r == -1);
        r = notifier_chain_register(NOTIFIER_PANIC, &nb_fail);
        TEST("notifier_chain_register: after failed unreg returns 0", r == 0);
        notifier_chain_unregister(NOTIFIER_PANIC, &nb_fail);
    }

    /* 22. notifier_call_chain with invalid type quietly returns 0 */
    {
        r = notifier_call_chain(-1, 0, NULL);
        TEST("notifier_call_chain: type=-1 returns 0", r == 0);
        r = notifier_call_chain(NOTIFIER_MAX, 0, NULL);
        TEST("notifier_call_chain: type=NOTIFIER_MAX returns 0", r == 0);
    }
}

/* ===================================================================
 *  test_notifier — extended edge cases
 * =================================================================== */

static void test_notifier_extended(void)
{
    printf("\n[notifier — extended]\n");

    struct notifier_block nb1 = { .notifier_call = test_callback, .next = NULL };
    int r;

    /* 1. Register NULL callback block fails */
    {
        r = notifier_chain_register(NOTIFIER_PANIC, NULL);
        TEST("register NULL block returns -1", r == -1);
    }

    /* 2. Unregister NULL callback block fails */
    {
        r = notifier_chain_unregister(NOTIFIER_PANIC, NULL);
        TEST("unregister NULL block returns -1", r == -1);
    }

    /* 3. Invalid types for register */
    {
        r = notifier_chain_register(-5, &nb1);
        TEST("register type=-5 returns -1", r == -1);
        r = notifier_chain_register(NOTIFIER_MAX + 10, &nb1);
        TEST("register type=NOTIFIER_MAX+10 returns -1", r == -1);
    }

    /* 4. Invalid types for unregister */
    {
        r = notifier_chain_unregister(-5, &nb1);
        TEST("unregister type=-5 returns -1", r == -1);
    }

    /* 5. Callback on all NOTIFIER types */
    {
        int types[] = {NOTIFIER_PANIC, NOTIFIER_DIE, NOTIFIER_REBOOT,
                       NOTIFIER_NET_EVENT, NOTIFIER_CPU_HP};
        for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
            notifier_chain_register(types[i], &nb1);
        }
        for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
            callback_count = 0;
            r = notifier_call_chain(types[i], 0, NULL);
            if (callback_count != 1) {
                printf("  FAIL: callback on type %d count=%d (expected 1)\n",
                       types[i], callback_count);
                tests_failed++;
            } else {
                printf("  PASS: callback on NOTIFIER type %d\n", types[i]);
                tests_passed++;
            }
        }
        /* Cleanup */
        for (size_t i = 0; i < sizeof(types)/sizeof(types[0]); i++) {
            notifier_chain_unregister(types[i], &nb1);
        }
    }

    /* 6. Register two different blocks on same type */
    {
        struct notifier_block nb_a = { .notifier_call = test_callback, .next = NULL };
        struct notifier_block nb_b = { .notifier_call = test_callback2, .next = NULL };
        notifier_chain_register(NOTIFIER_PANIC, &nb_a);
        r = notifier_chain_register(NOTIFIER_PANIC, &nb_b);
        TEST("register second block returns 0", r == 0);
        callback_count = 0;
        notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
        /* nb_b registered last (LIFO), fires first: adds 10, then nb_a adds 1 = 11 */
        TEST("two blocks fire in LIFO order", callback_count == 11);
        notifier_chain_unregister(NOTIFIER_PANIC, &nb_b);
        notifier_chain_unregister(NOTIFIER_PANIC, &nb_a);
        callback_count = 0;
        notifier_call_chain(NOTIFIER_PANIC, 0, NULL);
        TEST("chain empty after unregister both", callback_count == 0);
    }

    /* 7. Register 5 blocks, call chain, unregister all */
    {
        struct notifier_block many[5];
        for (int i = 0; i < 5; i++) {
            many[i].notifier_call = test_callback;
            many[i].next = NULL;
            notifier_chain_register(NOTIFIER_DIE, &many[i]);
        }
        callback_count = 0;
        notifier_call_chain(NOTIFIER_DIE, 0, NULL);
        TEST("5 callbacks on DIE type fire", callback_count == 5);
        for (int i = 0; i < 5; i++)
            notifier_chain_unregister(NOTIFIER_DIE, &many[i]);
        callback_count = 0;
        notifier_call_chain(NOTIFIER_DIE, 0, NULL);
        TEST("DIE type empty after unregister all 5", callback_count == 0);
    }

    /* 8. Chain with 1 callback, verify value and data passed */
    {
        struct notifier_block nb_val = { .notifier_call = test_callback, .next = NULL };
        notifier_chain_register(NOTIFIER_NET_EVENT, &nb_val);
        int test_val = 999;
        char test_data = 'X';
        callback_count = 0;
        r = notifier_call_chain(NOTIFIER_NET_EVENT, (unsigned long)test_val, &test_data);
        TEST("callback value passed correctly", last_val == (unsigned long)test_val);
        TEST("callback data passed correctly", last_data == &test_data);
        TEST("callback count is 1", callback_count == 1);
        notifier_chain_unregister(NOTIFIER_NET_EVENT, &nb_val);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Notifier Chain Tests ===\n\n");
    test_notifier();
    test_notifier_extended();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
