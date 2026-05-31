/*
 * workqueue.c — Deferred work execution
 *
 * Provides a kernel thread that processes work items FIFO.
 * Work can be scheduled from any context (including IRQ handlers).
 */

#include "workqueue.h"
#include "process.h"
#include "scheduler.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

static struct {
    work_fn_t fn;
    void     *arg;
    int       pending;  /* 1 = slot occupied */
} g_work[WORKQUEUE_MAX];

static spinlock_t g_wq_lock;
static int g_wq_initialized = 0;
static int g_wq_draining = 0;
static struct process *g_wq_thread = NULL;

/* Forward: worker thread entry */
static void workqueue_worker(void *arg);

void workqueue_init(void) {
    memset(g_work, 0, sizeof(g_work));
    spinlock_init(&g_wq_lock);
    g_wq_initialized = 1;

    /* Create the worker kernel thread */
    g_wq_thread = kthread_create(workqueue_worker, NULL, "workqueue");
    if (g_wq_thread)
        kprintf("[OK] Workqueue initialized\n");
    else
        kprintf("[!!] Workqueue: failed to create worker thread\n");
}

int workqueue_schedule(work_fn_t fn, void *arg) {
    if (!fn || !g_wq_initialized) return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_wq_lock, &irq_flags);

    int slot = -1;
    for (int i = 0; i < WORKQUEUE_MAX; i++) {
        if (!g_work[i].pending) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spinlock_irqsave_release(&g_wq_lock, irq_flags);
        return -1;
    }

    g_work[slot].fn = fn;
    g_work[slot].arg = arg;
    g_work[slot].pending = 1;

    spinlock_irqsave_release(&g_wq_lock, irq_flags);
    return slot;
}

void workqueue_drain(void) {
    if (!g_wq_initialized) return;

    g_wq_draining = 1;

    /* Spin until all work is processed. In a real kernel we'd use
     * a completion, but for simplicity we just yield and check. */
    for (;;) {
        int all_done = 1;
        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_wq_lock, &irq_flags);
        for (int i = 0; i < WORKQUEUE_MAX; i++) {
            if (g_work[i].pending) {
                all_done = 0;
                break;
            }
        }
        spinlock_irqsave_release(&g_wq_lock, irq_flags);
        if (all_done) break;
        scheduler_yield();
    }

    g_wq_draining = 0;
}

static void workqueue_worker(void *arg) {
    (void)arg;

    for (;;) {
        /* Find and execute the next pending work item (FIFO order) */
        work_fn_t fn = NULL;
        void *fn_arg = NULL;

        uint64_t irq_flags;
        spinlock_irqsave_acquire(&g_wq_lock, &irq_flags);

        for (int i = 0; i < WORKQUEUE_MAX; i++) {
            if (g_work[i].pending) {
                fn = g_work[i].fn;
                fn_arg = g_work[i].arg;
                g_work[i].pending = 0;
                break;
            }
        }

        spinlock_irqsave_release(&g_wq_lock, irq_flags);

        if (fn) {
            fn(fn_arg);
        } else {
            /* No work — yield to let other processes run */
            scheduler_yield();
        }
    }
}
