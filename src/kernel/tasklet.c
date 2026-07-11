#include "tasklet.h"
#include "printf.h"
#include "string.h"
#include "softirq.h"
#include "spinlock.h"

#define TASKLET_MAX 32

static struct tasklet_struct *tasklet_list[TASKLET_MAX];
static int tasklet_count = 0;

/* Protects tasklet_list, tasklet_count, and tasklet state transitions.
 * Must be acquired with interrupts disabled (spinlock_irqsave_acquire)
 * since tasklet_handler() runs in softirq context and may be invoked
 * from within IRQ processing. */
static spinlock_t tasklet_lock = SPINLOCK_INIT;

/* ── Softirq handler: run all scheduled tasklets ──────────────── */
static void tasklet_handler(void)
{
    struct tasklet_struct *todo[TASKLET_MAX];
    int nr = 0;
    uint64_t flags;

    /* Snapshot the list of runnable tasklets under the lock so that
     * concurrent tasklet_schedule() / tasklet_kill() cannot corrupt
     * the iteration.  Tasklet funcs run outside the lock so they can
     * re-schedule themselves (the common pattern). */
    spinlock_irqsave_acquire(&tasklet_lock, &flags);
    for (int i = 0; i < tasklet_count; i++) {
        struct tasklet_struct *t = tasklet_list[i];
        if (t && t->state) {
            t->state = 0;
            todo[nr++] = t;
        }
    }
    spinlock_irqsave_release(&tasklet_lock, flags);

    for (int i = 0; i < nr; i++) {
        if (todo[i]->func)
            todo[i]->func(todo[i]->data);
    }
}

/* ── Initialisation ──────────────────────────────────────────── */
void tasklet_init(void)
{
    memset(tasklet_list, 0, sizeof(tasklet_list));
    tasklet_count = 0;
    spinlock_init(&tasklet_lock);
    softirq_register(SOFTIRQ_TASKLET, tasklet_handler);
    kprintf("[OK] Tasklets initialized\n");
}

/* ── Schedule a tasklet for execution ─────────────────────────── *
 * Returns 0 on success, -1 if the tasklet list is full or t is
 * NULL.  Safe to call from any context (IRQ, process, softirq). */
int tasklet_schedule(struct tasklet_struct *t)
{
    uint64_t flags;
    int ret = 0;

    if (!t)
        return -1;

    spinlock_irqsave_acquire(&tasklet_lock, &flags);
    if (t->state) {
        /* Already scheduled — nothing to do. */
        goto out;
    }
    if (tasklet_count >= TASKLET_MAX) {
        ret = -1;
        goto out;
    }
    t->state = 1;
    tasklet_list[tasklet_count++] = t;
    softirq_raise(SOFTIRQ_TASKLET);

out:
    spinlock_irqsave_release(&tasklet_lock, flags);
    return ret;
}

/* ── Kill (deactivate + remove) a tasklet ─────────────────────── *
 * Removes @t from the scheduling list and clears its state flag.
 * If the tasklet is currently executing on another CPU this function
 * does NOT wait for it to finish — the caller must provide its own
 * completion synchronisation for that case.  Returns 0 on success,
 * -1 if @t is NULL or was not found in the list. */
int tasklet_kill(struct tasklet_struct *t)
{
    uint64_t flags;
    int found = 0;

    if (!t)
        return -1;

    spinlock_irqsave_acquire(&tasklet_lock, &flags);

    /* Clear the state so the handler will skip it. */
    t->state = 0;

    /* Remove from the list so it cannot be re-dispatched. */
    for (int i = 0; i < tasklet_count; i++) {
        if (tasklet_list[i] == t) {
            /* Shift remaining entries left. */
            int remain = tasklet_count - i - 1;
            if (remain > 0)
                __builtin_memmove(&tasklet_list[i],
                                  &tasklet_list[i + 1],
                                  remain * sizeof(tasklet_list[0]));
            tasklet_list[tasklet_count - 1] = NULL;
            tasklet_count--;
            found = 1;
            break;
        }
    }

    spinlock_irqsave_release(&tasklet_lock, flags);
    return found ? 0 : -1;
}

/* ── Stub: tasklet_hi_schedule ─────────────────────────────────── */
int tasklet_hi_schedule(struct tasklet_struct *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_hi_schedule: not yet implemented\n");
    return -1;
}

/* ── Stub: tasklet_enable ─────────────────────────────────────── */
int tasklet_enable(struct tasklet_struct *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_enable: not yet implemented\n");
    return -1;
}

/* ── Stub: tasklet_disable ────────────────────────────────────── */
int tasklet_disable(struct tasklet_struct *t)
{
    (void)t;
    kprintf("[tasklet] tasklet_disable: not yet implemented\n");
    return -1;
}
