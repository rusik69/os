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
 *
 * GRUB (Greedy Reclamation of Unused Bandwidth):
 *   If a deadline task blocks or finishes before using its full budget,
 *   the unused portion is added to a per-CPU reclaim pool.  Other
 *   deadline tasks that have exhausted their own budget may continue
 *   running by drawing from this reclaim pool, improving overall CPU
 *   utilisation without missing deadlines.
 */

/* Maximum number of SCHED_DEADLINE tasks per CPU */
#define SCHED_DL_MAX_PER_CPU 8

/*
 * Per-CPU deadline runqueue — tracks deadline tasks, total utilisation,
 * and the GRUB reclaim pool of unused bandwidth.
 */
struct cpu_dl_rq {
    struct process *tasks[SCHED_DL_MAX_PER_CPU];
    int nr_tasks;
    uint64_t total_bw;          /* sum(dl_runtime / dl_period) << BW_SHIFT */
    uint64_t reclaimed_bw;      /* GRUB reclaim pool (fixed-point, same unit) */
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

/* Check if a DL task is runnable (has budget, not throttled, or can reclaim) */
int sched_deadline_is_runnable(struct process *proc);

/* Called when a deadline task is voluntarily descheduled (blocks / yields).
 * Captures unused budget and adds it to the per-CPU reclaim pool (GRUB). */
void sched_deadline_task_blocked(struct process *proc);

/* Update absolute deadline for a task (called at period start) */
void sched_deadline_update_deadline(struct process *proc);

/* Debug: print deadline runqueue state for a given CPU */
void sched_deadline_dump(int cpu);

#endif /* SCHED_DEADLINE_H */
