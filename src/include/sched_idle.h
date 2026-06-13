#ifndef SCHED_IDLE_H
#define SCHED_IDLE_H

#include "types.h"
#include "process.h"

/*
 * SCHED_IDLE — lowest-priority scheduling class
 *
 * Idle-class tasks run only when no other scheduling class
 * (RT, CFS, deadline) has a runnable task.
 */

/* Initialise the per-CPU idle runqueue for a CPU */
void sched_idle_init_cpu(int cpu);

/* Add a process to the idle scheduling class. Returns 0 on success. */
int sched_idle_enqueue(struct process *proc);

/* Remove a process from the idle scheduling class. Returns 0 on success. */
int sched_idle_dequeue(struct process *proc);

/* Pick the next idle task to run on the current CPU. Returns NULL if none. */
struct process *sched_idle_select(void);

/* Called from scheduler_tick() for a running SCHED_IDLE task. */
void sched_idle_tick(struct process *proc);

/* Voluntary yield for idle-class tasks. */
void sched_idle_yield(struct process *proc);

/* Return number of idle tasks on the current CPU. */
int sched_idle_count(void);

#endif /* SCHED_IDLE_H */
