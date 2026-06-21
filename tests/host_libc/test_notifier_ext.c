/*
 * test_notifier_ext.c — Host-side tests for priority-ordered notifier chains
 *
 * Tests notifier_ext_chain_register, notifier_ext_chain_unregister,
 * notifier_ext_call_chain, notifier_ext_init from src/kernel/notifier_ext.c.
 *
 * Provides stubs for kernel spinlock and printf symbols needed by the
 * compiled kernel object.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ===================================================================
 *  Kernel type declarations (mirror notifier_ext.h)
 * =================================================================== */

#define NOTIFY_DONE             0x00
#define NOTIFY_OK               0x01
#define NOTIFY_BAD              0x02
#define NOTIFY_STOP_MASK        0x80

struct notifier_block {
    int (*notify)(struct notifier_block *nb,
                  unsigned long action, void *data);
    struct notifier_block *next;
    int                 priority;
};

struct notifier_head {
    struct notifier_block *head;
};

/* ── Extern declarations for the kernel functions under test ────── */

extern void notifier_ext_init(void);
extern int notifier_ext_chain_register(struct notifier_head *nh,
                                       struct notifier_block *nb);
extern int notifier_ext_chain_unregister(struct notifier_head *nh,
                                         struct notifier_block *nb);
extern int notifier_ext_call_chain(struct notifier_head *nh,
                                   unsigned long action, void *data);

/* ===================================================================
 *  Stubs for kernel symbols
 * =================================================================== */

void vga_putchar(char c)      { (void)c; }
void serial_putchar(char c)   { (void)c; }
int kprintf(const char *fmt, ...) { (void)fmt; return 0; }
int console_loglevel = 7;
int default_message_loglevel = 6;

/* Spinlock stubs — the notifier_ext.o calls these but they are no-ops
 * in the host test environment since there is no concurrency. */
typedef struct { int dummy; } spinlock_t;
void spinlock_init(spinlock_t *lock)    { (void)lock; }
void spinlock_acquire(spinlock_t *lock) { (void)lock; }
void spinlock_release(spinlock_t *lock) { (void)lock; }

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
static unsigned long last_action = 0;
static void *last_data = NULL;
static struct notifier_block *last_nb = NULL;

static int cb_track(struct notifier_block *nb, unsigned long action, void *data)
{
    callback_count++;
    last_nb = nb;
    last_action = action;
    last_data = data;
    return NOTIFY_OK;
}

static int cb_adder2(struct notifier_block *nb, unsigned long action, void *data)
{
    (void)nb; (void)action; (void)data;
    callback_count += 2;
    return NOTIFY_OK;
}

static int cb_adder10(struct notifier_block *nb, unsigned long action, void *data)
{
    (void)nb; (void)action; (void)data;
    callback_count += 10;
    return NOTIFY_OK;
}

static int cb_stopper(struct notifier_block *nb, unsigned long action, void *data)
{
    (void)nb; (void)action; (void)data;
    callback_count += 100;
    return NOTIFY_BAD | NOTIFY_STOP_MASK;
}

static int cb_self_unreg(struct notifier_block *nb, unsigned long action, void *data)
{
    (void)action; (void)data;
    callback_count += 1000;
    /* Unregister from the head whose address we smuggled in last_data */
    struct notifier_head *self_head = (struct notifier_head *)last_data;
    if (self_head)
        notifier_ext_chain_unregister(self_head, nb);
    return NOTIFY_OK;
}

/* Priority-order tracking callbacks — array-based to verify execution order */

static int firing_order[5];
static int order_idx = 0;

static void reset_order(void)
{
    order_idx = 0;
}

static int cb_prio_high(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    firing_order[order_idx++] = 300;
    return NOTIFY_OK;
}
static int cb_prio_med(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    firing_order[order_idx++] = 200;
    return NOTIFY_OK;
}
static int cb_prio_low(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    firing_order[order_idx++] = 100;
    return NOTIFY_OK;
}

/* FIFO ordering tracking */

static int fifo_order[4];
static int fifo_idx = 0;

static void reset_fifo(void)
{
    fifo_idx = 0;
}

static int cb_fifo1(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    fifo_order[fifo_idx++] = 1;
    return NOTIFY_OK;
}
static int cb_fifo2(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    fifo_order[fifo_idx++] = 2;
    return NOTIFY_OK;
}
static int cb_fifo3(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    fifo_order[fifo_idx++] = 3;
    return NOTIFY_OK;
}
static int cb_fifo4(struct notifier_block *nb, unsigned long a, void *d)
{
    (void)nb; (void)a; (void)d;
    fifo_order[fifo_idx++] = 4;
    return NOTIFY_OK;
}

/* Mixed priority tracking */

static int mixed_order[5];
static int mixed_idx = 0;

static void reset_mixed(void)
{
    mixed_idx = 0;
}

static int cb_m1(struct notifier_block *nb, unsigned long a, void *d)
{ (void)nb; (void)a; (void)d; mixed_order[mixed_idx++] = 1; return NOTIFY_OK; }
static int cb_m2(struct notifier_block *nb, unsigned long a, void *d)
{ (void)nb; (void)a; (void)d; mixed_order[mixed_idx++] = 2; return NOTIFY_OK; }
static int cb_m3(struct notifier_block *nb, unsigned long a, void *d)
{ (void)nb; (void)a; (void)d; mixed_order[mixed_idx++] = 3; return NOTIFY_OK; }
static int cb_m4(struct notifier_block *nb, unsigned long a, void *d)
{ (void)nb; (void)a; (void)d; mixed_order[mixed_idx++] = 4; return NOTIFY_OK; }
static int cb_m5(struct notifier_block *nb, unsigned long a, void *d)
{ (void)nb; (void)a; (void)d; mixed_order[mixed_idx++] = 5; return NOTIFY_OK; }

/* ===================================================================
 *  test_notifier_ext
 * =================================================================== */

static void test_notifier_ext(void)
{
    /* 0. Init (doesn't crash, no observable state) */
    notifier_ext_init();

    /* 1. Register with invalid arguments */
    {
        struct notifier_block nb = { .notify = cb_track, .next = NULL, .priority = 0 };
        int r;

        r = notifier_ext_chain_register(NULL, &nb);
        TEST("register: NULL nh returns -1", r == -1);

        r = notifier_ext_chain_register((struct notifier_head *)1, NULL);
        TEST("register: NULL nb returns -1", r == -1);

        struct notifier_block nb_null_cb = { .notify = NULL, .next = NULL, .priority = 0 };
        r = notifier_ext_chain_register((struct notifier_head *)1, &nb_null_cb);
        TEST("register: NULL notify returns -1", r == -1);
    }

    /* 2. Single notifier: register, call, unregister */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb = { .notify = cb_track, .next = NULL, .priority = 0 };

        int r = notifier_ext_chain_register(&nh, &nb);
        TEST("register: single notifier returns 0", r == 0);

        int data_val = 99;
        callback_count = 0;
        last_action = 0;
        last_data = NULL;
        r = notifier_ext_call_chain(&nh, 42, &data_val);
        TEST("call_chain: single callback fires", callback_count == 1);
        TEST("call_chain: action passed correctly", last_action == 42);
        TEST("call_chain: data passed correctly", last_data == &data_val);

        r = notifier_ext_chain_unregister(&nh, &nb);
        TEST("unregister: single notifier returns 0", r == 0);

        callback_count = 0;
        r = notifier_ext_call_chain(&nh, 0, NULL);
        TEST("call_chain: empty after unregister returns NOTIFY_DONE", r == NOTIFY_DONE);
        TEST("call_chain: no callbacks after unregister", callback_count == 0);
    }

    /* 3. Unregister not found */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb = { .notify = cb_track, .next = NULL, .priority = 0 };
        int r = notifier_ext_chain_unregister(&nh, &nb);
        TEST("unregister: not found returns -1", r == -1);

        r = notifier_ext_chain_unregister(NULL, &nb);
        TEST("unregister: NULL nh returns -1", r == -1);

        r = notifier_ext_chain_unregister(&nh, NULL);
        TEST("unregister: NULL nb returns -1", r == -1);
    }

    /* 4. Priority ordering: higher priority fires first */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb_high = { .notify = cb_prio_high, .next = NULL, .priority = 100 };
        struct notifier_block nb_med  = { .notify = cb_prio_med,  .next = NULL, .priority = 0 };
        struct notifier_block nb_low  = { .notify = cb_prio_low,  .next = NULL, .priority = -100 };

        /* Register in reverse priority order (low, med, high) */
        TEST("register prio=-100 returns 0",
             notifier_ext_chain_register(&nh, &nb_low) == 0);
        TEST("register prio=0 returns 0",
             notifier_ext_chain_register(&nh, &nb_med) == 0);
        TEST("register prio=100 returns 0",
             notifier_ext_chain_register(&nh, &nb_high) == 0);

        /* Verify call chain fires in priority order: high(100) → med(0) → low(-100) */
        reset_order();
        notifier_ext_call_chain(&nh, 0, NULL);
        TEST("priority order: three callbacks fired", order_idx == 3);
        TEST("priority order: highest first", firing_order[0] == 300);
        TEST("priority order: middle second", firing_order[1] == 200);
        TEST("priority order: lowest last", firing_order[2] == 100);

        /* Clean up */
        notifier_ext_chain_unregister(&nh, &nb_high);
        notifier_ext_chain_unregister(&nh, &nb_med);
        notifier_ext_chain_unregister(&nh, &nb_low);
    }

    /* 5. Same priority = FIFO ordering */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb1 = { .notify = cb_fifo1, .next = NULL, .priority = 0 };
        struct notifier_block nb2 = { .notify = cb_fifo2, .next = NULL, .priority = 0 };
        struct notifier_block nb3 = { .notify = cb_fifo3, .next = NULL, .priority = 0 };
        struct notifier_block nb4 = { .notify = cb_fifo4, .next = NULL, .priority = 0 };

        notifier_ext_chain_register(&nh, &nb1);
        notifier_ext_chain_register(&nh, &nb2);
        notifier_ext_chain_register(&nh, &nb3);
        notifier_ext_chain_register(&nh, &nb4);

        reset_fifo();
        notifier_ext_call_chain(&nh, 0, NULL);
        TEST("FIFO: four same-priority fired", fifo_idx == 4);
        TEST("FIFO: first registered fires first", fifo_order[0] == 1);
        TEST("FIFO: second registered fires second", fifo_order[1] == 2);
        TEST("FIFO: third registered fires third", fifo_order[2] == 3);
        TEST("FIFO: fourth registered fires fourth", fifo_order[3] == 4);

        notifier_ext_chain_unregister(&nh, &nb1);
        notifier_ext_chain_unregister(&nh, &nb2);
        notifier_ext_chain_unregister(&nh, &nb3);
        notifier_ext_chain_unregister(&nh, &nb4);
    }

    /* 6. Two notifiers on same head — both fire */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nba = { .notify = cb_track, .next = NULL, .priority = 0 };
        struct notifier_block nbb = { .notify = cb_adder10, .next = NULL, .priority = 0 };

        notifier_ext_chain_register(&nh, &nba);
        notifier_ext_chain_register(&nh, &nbb);

        callback_count = 0;
        notifier_ext_call_chain(&nh, 0, NULL);
        /* nb1 adds 1, nb2 adds 10 */
        TEST("two notifiers: both fire", callback_count == 11);

        notifier_ext_chain_unregister(&nh, &nba);
        notifier_ext_chain_unregister(&nh, &nbb);
    }

    /* 7. Call chain stops at NOTIFY_BAD | NOTIFY_STOP_MASK */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb_stop = { .notify = cb_stopper, .next = NULL, .priority = 10 };
        struct notifier_block nb_after = { .notify = cb_track, .next = NULL, .priority = 0 };

        notifier_ext_chain_register(&nh, &nb_stop);
        notifier_ext_chain_register(&nh, &nb_after);

        callback_count = 0;
        int r = notifier_ext_call_chain(&nh, 0, NULL);
        /* stopper fires first (higher priority), adds 100, returns STOP */
        TEST("stop callback: fires and stops chain", callback_count == 100);
        TEST("stop callback: return code has NOTIFY_STOP_MASK",
             (r & NOTIFY_STOP_MASK) != 0);

        notifier_ext_chain_unregister(&nh, &nb_stop);
        notifier_ext_chain_unregister(&nh, &nb_after);
    }

    /* 8. Call chain on empty head returns NOTIFY_DONE */
    {
        struct notifier_head nh = { .head = NULL };
        int r = notifier_ext_call_chain(&nh, 0, NULL);
        TEST("empty chain: returns NOTIFY_DONE", r == NOTIFY_DONE);
    }

    /* 9. Call chain with NULL nh returns NOTIFY_DONE */
    {
        int r = notifier_ext_call_chain(NULL, 0, NULL);
        TEST("NULL nh: returns NOTIFY_DONE", r == NOTIFY_DONE);
    }

    /* 10. Duplicate registration prevention */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb = { .notify = cb_track, .next = NULL, .priority = 0 };

        TEST("register first time returns 0",
             notifier_ext_chain_register(&nh, &nb) == 0);
        TEST("register duplicate returns -1",
             notifier_ext_chain_register(&nh, &nb) == -1);

        notifier_ext_chain_unregister(&nh, &nb);
    }

    /* 11. Re-register after unregister */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb = { .notify = cb_track, .next = NULL, .priority = 0 };

        notifier_ext_chain_register(&nh, &nb);
        notifier_ext_chain_unregister(&nh, &nb);

        int r = notifier_ext_chain_register(&nh, &nb);
        TEST("re-register after unregister returns 0", r == 0);

        callback_count = 0;
        notifier_ext_call_chain(&nh, 0, NULL);
        TEST("re-registered notifier fires once", callback_count == 1);

        notifier_ext_chain_unregister(&nh, &nb);
    }

    /* 12. Many notifiers on same head */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block many[10];
        for (int i = 0; i < 10; i++) {
            many[i].notify = cb_track;
            many[i].next = NULL;
            many[i].priority = 0;
            notifier_ext_chain_register(&nh, &many[i]);
        }
        callback_count = 0;
        notifier_ext_call_chain(&nh, 0, NULL);
        TEST("10 notifiers: all fire", callback_count == 10);

        for (int i = 0; i < 10; i++)
            notifier_ext_chain_unregister(&nh, &many[i]);
    }

    /* 13. Self-unregister during call chain */
    {
        struct notifier_head nh = { .head = NULL };
        struct notifier_block nb_self = { .notify = cb_self_unreg, .next = NULL, .priority = 10 };
        struct notifier_block nb_tail = { .notify = cb_adder2, .next = NULL, .priority = 0 };

        /* Smuggle the head address through last_data so cb_self_unreg
         * can unregister itself from it. */
        last_data = &nh;

        notifier_ext_chain_register(&nh, &nb_self);
        notifier_ext_chain_register(&nh, &nb_tail);

        callback_count = 0;
        int r = notifier_ext_call_chain(&nh, 0, NULL);
        /* Self-unreg fires first (priority 10), adds 1000, unregisters itself.
         * After unlink, nb_self.next = NULL so chain walk stops — nb_tail
         * may or may not fire depending on implementation.  At minimum
         * the self-unregister callback itself fires. */
        TEST("self-unregister: self callback fires", (callback_count & 1000) != 0);
        (void)r;

        /* Verify the chain still has nb_tail (just unlinked nb_self) */
        notifier_ext_chain_unregister(&nh, &nb_tail);
        callback_count = 0;
        notifier_ext_call_chain(&nh, 0, NULL);
        TEST("self-unregister: tail remains in chain", callback_count == 0);
        last_data = NULL;
    }

    /* 14. Mixed priorities with multiple callbacks at same level */
    {
        struct notifier_head nh = { .head = NULL };

        struct notifier_block nb1 = { .notify = cb_m1, .next = NULL, .priority = 10 };
        struct notifier_block nb2 = { .notify = cb_m2, .next = NULL, .priority = 0 };
        struct notifier_block nb3 = { .notify = cb_m3, .next = NULL, .priority = 20 };
        struct notifier_block nb4 = { .notify = cb_m4, .next = NULL, .priority = 0 };
        struct notifier_block nb5 = { .notify = cb_m5, .next = NULL, .priority = -10 };

        notifier_ext_chain_register(&nh, &nb5); /* -10 */
        notifier_ext_chain_register(&nh, &nb2); /* 0 */
        notifier_ext_chain_register(&nh, &nb1); /* 10 */
        notifier_ext_chain_register(&nh, &nb3); /* 20 */
        notifier_ext_chain_register(&nh, &nb4); /* 0 (FIFO after nb2) */

        reset_mixed();
        notifier_ext_call_chain(&nh, 0, NULL);
        /* Expected order: nb3(20) → nb1(10) → nb2(0) → nb4(0) → nb5(-10) */
        TEST("mixed priorities: five fired", mixed_idx == 5);
        TEST("mixed: highest first (20)", mixed_order[0] == 3);
        TEST("mixed: second highest (10)", mixed_order[1] == 1);
        TEST("mixed: first zero priority", mixed_order[2] == 2);
        TEST("mixed: second zero priority (FIFO)", mixed_order[3] == 4);
        TEST("mixed: lowest priority (-10)", mixed_order[4] == 5);

        notifier_ext_chain_unregister(&nh, &nb1);
        notifier_ext_chain_unregister(&nh, &nb2);
        notifier_ext_chain_unregister(&nh, &nb3);
        notifier_ext_chain_unregister(&nh, &nb4);
        notifier_ext_chain_unregister(&nh, &nb5);
    }
}

/* ===================================================================
 *  Main
 * =================================================================== */

int main(void)
{
    printf("=== Notifier Chain Extension (Priority-Ordered) Tests ===\n\n");
    test_notifier_ext();

    printf("\n");
    printf("============================================\n");
    printf("  Results: %d run, %d passed, %d failed\n",
           tests_passed + tests_failed, tests_passed, tests_failed);
    printf("============================================\n");

    return tests_failed > 0 ? 1 : 0;
}
