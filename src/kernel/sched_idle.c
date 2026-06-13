/*
 * sched_idle.c — SCHED_IDLE scheduling class
 *
 * Implements the SCHED_IDLE policy: tasks that only run when
 * no other runnable task (RT, CFS, deadline) needs the CPU.
 *
 * This is the lowest-priority class. Idle tasks are always
 * eligible to run but are selected only after all other
 * scheduling classes have declined.
 *
 * B4: SCHED_IDLE enhancement — standalone idle scheduling class.
 *
 * Integration:
 *   - sched_idle_enqueue()   — add a process to the idle runqueue
 *   - sched_idle_dequeue()   — remove a process from the idle runqueue
 *   - sched_idle_select()    — pick the next idle task (lowest priority)
 *   - sched_idle_tick()      — called on timer tick for idle tasks
 *   - sched_idle_yield()     — voluntary yield of idle-class task
 *
 * The main scheduler (scheduler.c) calls sched_idle_select() when
 * no RT, deadline, or CFS task is available.
 */

#include "scheduler.h"
#include "process.h"
#include "smp.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"

/* ── Per-CPU idle runqueue ────────────────────────────────────────────── */

#define SCHED_IDLE_MAX_PER_CPU 16

struct cpu_idle_rq {
    struct process *tasks[SCHED_IDLE_MAX_PER_CPU];
    int             nr_tasks;
};

static struct cpu_idle_rq cpu_idle_rq[SMP_MAX_CPUS];
static spinlock_t idle_lock = SPINLOCK_INIT;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static inline struct cpu_idle_rq *this_idle_rq(void)
{
    int cpu = (int)get_cpu_id();
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        cpu = 0;
    return &cpu_idle_rq[cpu];
}

/* ── API ──────────────────────────────────────────────────────────────── */

/*
 * sched_idle_init_cpu — initialise the idle runqueue for a CPU
 */
void sched_idle_init_cpu(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;
    struct cpu_idle_rq *rq = &cpu_idle_rq[cpu];
    memset(rq, 0, sizeof(*rq));
}

/*
 * sched_idle_enqueue — add a process to the idle scheduling class
 *
 * Idle tasks are kept in a per-CPU list. Returns 0 on success,
 * -ENOSPC if the runqueue is full.
 */
int sched_idle_enqueue(struct process *proc)
{
    if (!proc)
        return -1;

    uint64_t flags;
    spinlock_irqsave_acquire(&idle_lock, &flags);

    /* Determine affinity: pick the CPU from the process mask or current CPU */
    int cpu = 0;
    if (proc->cpu_affinity) {
        /* Find first set bit in affinity mask */
        for (int i = 0; i < SMP_MAX_CPUS; i++) {
            if (proc->cpu_affinity & (1U << i)) {
                cpu = i;
                break;
            }
        }
    } else {
        cpu = (int)get_cpu_id();
        if (cpu < 0 || cpu >= SMP_MAX_CPUS) cpu = 0;
    }

    struct cpu_idle_rq *rq = &cpu_idle_rq[cpu];
    if (rq->nr_tasks >= SCHED_IDLE_MAX_PER_CPU) {
        spinlock_irqsave_release(&idle_lock, flags);
        return -1;
    }

    rq->tasks[rq->nr_tasks++] = proc;
    proc->sched_policy = SCHED_IDLE;

    spinlock_irqsave_release(&idle_lock, flags);
    return 0;
}

/*
 * sched_idle_dequeue — remove a process from the idle runqueue
 */
int sched_idle_dequeue(struct process *proc)
{
    if (!proc)
        return -1;

    uint64_t flags;
    spinlock_irqsave_acquire(&idle_lock, &flags);

    for (int cpu = 0; cpu < SMP_MAX_CPUS; cpu++) {
        struct cpu_idle_rq *rq = &cpu_idle_rq[cpu];
        for (int i = 0; i < rq->nr_tasks; i++) {
            if (rq->tasks[i] == proc) {
                /* Compact the array */
                for (int j = i + 1; j < rq->nr_tasks; j++)
                    rq->tasks[j - 1] = rq->tasks[j];
                rq->nr_tasks--;
                spinlock_irqsave_release(&idle_lock, flags);
                return 0;
            }
        }
    }

    spinlock_irqsave_release(&idle_lock, flags);
    return -1;  /* Not found */
}

/*
 * sched_idle_select — pick the next idle-class task to run
 *
 * Called by schedule() when no other class has a runnable task.
 * Returns the best idle task for this CPU, or NULL if none available.
 *
 * Idle tasks run in round-robin order. They are guaranteed to run
 * only when the CPU would otherwise be idle.
 */
struct process *sched_idle_select(void)
{
    struct cpu_idle_rq *rq = this_idle_rq();

    if (rq->nr_tasks == 0)
        return NULL;

    /* Simple round-robin: pick the first task and rotate */
    struct process *next = rq->tasks[0];

    /* Rotate the queue */
    for (int i = 1; i < rq->nr_tasks; i++)
        rq->tasks[i - 1] = rq->tasks[i];
    rq->tasks[rq->nr_tasks - 1] = next;

    return next;
}

/*
 * sched_idle_tick — called from scheduler_tick for an idle-class task
 *
 * Idle tasks have no time-slice enforcement — they run only when
 * nothing else wants the CPU. The tick simply returns, allowing
 * the normal scheduler tick to check for preemption.
 */
void sched_idle_tick(struct process *proc)
{
    (void)proc;
    /* Idle tasks are not preempted by time slice expiry.
     * The main scheduler will choose something else if a
     * higher-priority task becomes runnable. */
}

/*
 * sched_idle_yield — voluntary yield called from sys_sched_yield
 *
 * Since idle tasks are lowest priority, yielding immediately
 * triggers a full reschedule, giving other classes a chance.
 */
void sched_idle_yield(struct process *proc)
{
    (void)proc;
    schedule();
}

/*
 * sched_idle_count — return number of idle tasks on current CPU
 */
int sched_idle_count(void)
{
    return this_idle_rq()->nr_tasks;
}
