#ifndef IRQ_WORK_H
#define IRQ_WORK_H

#include "types.h"
#include "llist.h"

/*
 * IRQ work: a deferred-work mechanism executed in interrupt context.
 *
 * Each work item is an llist_node embedded in a struct irq_work.
 * Work is queued on a given CPU (or the current CPU) and executed
 * in IRQ context (typically from a timer tick or softirq).
 *
 * Based on the Linux irq_work concept.
 */

/* Flags for irq_work.flags */
#define IRQ_WORK_PENDING    1
#define IRQ_WORK_BUSY       2
#define IRQ_WORK_LAZY       4   /* defer if IRQ context is not available */

struct irq_work;

typedef void (*irq_work_func_t)(struct irq_work *work);

struct irq_work {
    struct llist_node   llnode;    /* for lock-less queueing */
    irq_work_func_t     func;      /* callback */
    int                 flags;     /* status flags */
};

/*
 * init_irq_work  - Initialise an irq_work item.
 */
static inline void init_irq_work(struct irq_work *work,
                                 irq_work_func_t func)
{
    work->llnode.next = NULL;
    work->func        = func;
    work->flags       = 0;
}

/*
 * irq_work_queue  - Queue @work on the current CPU.
 * Returns 1 if the work was successfully queued, 0 if it was
 * already pending.
 */
int irq_work_queue(struct irq_work *work);

/*
 * irq_work_queue_on  - Queue @work on a specific @cpu.
 * Returns 1 if queued, 0 if already pending (and not touched).
 */
int irq_work_queue_on(struct irq_work *work, int cpu);

/*
 * irq_work_run  - Execute all pending work items on the current CPU.
 * Call this from IRQ context (e.g. timer IRQ).
 */
void irq_work_run(void);

/*
 * irq_work_sync  - Wait for all pending work on the current CPU
 * to complete.  Busy-waits if work is currently being executed
 * on another CPU.
 */
void irq_work_sync(struct irq_work *work);

void irq_work_init(void);

/* ── CPU hotplug integration ───────────────────────────────────── */

/*
 * irq_work_cpu_offline - Drain pending IRQ work from a CPU going offline.
 * Must be called from the hotplug path before the CPU state transitions
 * to OFFLINE. Moves orphaned work items to the calling CPU's queue.
 */
void irq_work_cpu_offline(int cpu);

/*
 * irq_work_cpu_online - Re-initialise IRQ work state for a CPU coming
 * online. Must be called before the CPU state transitions to ONLINE.
 */
void irq_work_cpu_online(int cpu);

#endif /* IRQ_WORK_H */
