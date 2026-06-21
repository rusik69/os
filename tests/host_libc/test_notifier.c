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
}

/* ===================================================================
 *  Main
 * =================================================================== */
int main(void)
{
    printf("=== Notifier Chain Tests ===\n\n");
    test_notifier();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
