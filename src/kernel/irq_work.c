#include "irq_work.h"
#include "printf.h"
#include "kernel.h"
#include "smp.h"
#include "spinlock.h"

/*
 * IRQ work queue — per-CPU llist-based deferred work.
 *
 * Each CPU has its own llist_head for pending work.  Queueing is
 * wait-free (cmpxchg on the llist).  Execution drains the list in FIFO
 * order.
 */

/* Per-CPU irq_work lists */
static struct llist_head irq_work_lists[SMP_MAX_CPUS];

/* Per-CPU lock to serialise execution */
static spinlock_t irq_work_locks[SMP_MAX_CPUS];

static struct llist_head *irq_work_list(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        cpu = 0;
    return &irq_work_lists[cpu];
}

static spinlock_t *irq_work_lock(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        cpu = 0;
    return &irq_work_locks[cpu];
}

int irq_work_queue(struct irq_work *work)
{
    int cpu = smp_get_cpu_id();
    return irq_work_queue_on(work, cpu);
}

int irq_work_queue_on(struct irq_work *work, int cpu)
{
    struct llist_head *list;

    if (!work || !work->func)
        return 0;

    /*
     * Atomically test-and-set the PENDING flag.
     * If it was already pending, do nothing.
     */
    if (__sync_lock_test_and_set(&work->flags, IRQ_WORK_PENDING) & IRQ_WORK_PENDING)
        return 0;

    list = irq_work_list(cpu);
    llist_add(&work->llnode, list);

    return 1;
}

void irq_work_run(void)
{
    int cpu = smp_get_cpu_id();
    struct llist_head *list = irq_work_list(cpu);
    struct llist_node *node, *next;
    struct irq_work *work;

    node = llist_del_all(list);
    if (!node)
        return;

    spinlock_acquire(irq_work_lock(cpu));

    /* The llist is singly-linked in reverse order (LIFO).
     * Reverse it for FIFO execution. */
    struct llist_node *rev = NULL;
    while (node) {
        next = node->next;
        node->next = rev;
        rev = node;
        node = next;
    }

    /* Execute each work item */
    node = rev;
    while (node) {
        work = llist_entry(node, struct irq_work, llnode);
        node = node->next;

        /* Clear PENDING, set BUSY, execute, clear BUSY */
        __sync_synchronize();
        work->flags = IRQ_WORK_BUSY;
        work->func(work);
        __sync_synchronize();
        work->flags = 0;
    }

    spinlock_release(irq_work_lock(cpu));
}

void irq_work_sync(struct irq_work *work)
{
    if (!work)
        return;

    /* Busy-wait until the work is no longer pending or busy.
     * In a real system this would use a completion. */
    while (work->flags & (IRQ_WORK_PENDING | IRQ_WORK_BUSY)) {
        __asm__ volatile("pause");
    }
}

void irq_work_init(void)
{
    for (int i = 0; i < SMP_MAX_CPUS; i++) {
        init_llist_head(&irq_work_lists[i]);
        spinlock_init(&irq_work_locks[i]);
    }
    kprintf("[OK] irq_work: IRQ work queues initialised\n");
}

/* ── Stub: irq_work_is_pending ─────────────────────────────── */
int irq_work_is_pending(void *work)
{
    (void)work;
    kprintf("[irq_work] irq_work_is_pending: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: irq_work_claim ─────────────────────────────── */
int irq_work_claim(void *work)
{
    (void)work;
    kprintf("[irq_work] irq_work_claim: not yet implemented\n");
    return -ENOSYS;
}
