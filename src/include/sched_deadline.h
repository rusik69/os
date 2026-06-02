#ifndef SCHED_DEADLINE_H
#define SCHED_DEADLINE_H

#include "types.h"
#include "process.h"

/*
 * SCHED_DEADLINE — Earliest Deadline First scheduling for real-time tasks.
 *
 * Each task declares its worst-case execution time (runtime), period,
 * and relative deadline.  The kernel guarantees that if the total
 * utilisation on a CPU is <= 1, every deadline task meets its timing
 * constraints.
 *
 * Budget replenishment follows the CBS (Constant Bandwidth Server) model:
 *   - When a task exhausts its runtime before its absolute deadline,
 *     it is throttled until the next period starts.
 *   - At the start of each period, runtime is replenished and a new
 *     absolute deadline is set.
 */

/* Maximum number of SCHED_DEADLINE tasks per CPU */
#define SCHED_DL_MAX_PER_CPU 8

/*
 * Per-CPU deadline runqueue — tracks deadline tasks and total utilisation.
 */
struct cpu_dl_rq {
    struct process *tasks[SCHED_DL_MAX_PER_CPU];
    int nr_tasks;
    uint64_t total_bw;          /* sum(dl_runtime / dl_period) << BW_SHIFT */
};

/* Fixed-point bandwidth precision */
#define DL_BW_SHIFT 20
#define DL_BW_UNIT  (1ULL << DL_BW_SHIFT)

/* ── API ─────────────────────────────────────────────────────────────── */

/* Initialise the per-CPU deadline runqueue for a CPU */
void sched_deadline_init_cpu(int cpu);

/* Try to add a task to the deadline runqueue. Returns 0 on success,
 * -EBUSY if admission control fails, -EINVAL for invalid params. */
int sched_deadline_add_task(struct process *proc);

/* Remove a task from the deadline runqueue */
void sched_deadline_remove_task(struct process *proc);

/* Pick the next deadline task to run (Earliest Deadline First).
 * Returns NULL if no deadline task is runnable. */
struct process *sched_deadline_pick_next(void);

/* Called from scheduler_tick for a running SCHED_DEADLINE task */
void sched_deadline_tick(struct process *proc);

/* Replenish throttled tasks whose next period has started */
void sched_deadline_replenish(void);

/* Check if a DL task is runnable (has budget, not throttled) */
int sched_deadline_is_runnable(struct process *proc);

/* Update absolute deadline for a task (called at period start) */
void sched_deadline_update_deadline(struct process *proc);

/* Debug: print deadline runqueue state for a given CPU */
void sched_deadline_dump(int cpu);

#endif /* SCHED_DEADLINE_H */
