/*
 * SCHED_DEADLINE — Earliest Deadline First with CBS budget enforcement
 *
 * Implements the Constant Bandwidth Server (CBS) model for real-time tasks:
 *   - Each task has a budget (dl_runtime) that is replenished every period.
 *   - Tasks are scheduled by Earliest Deadline First (EDF).
 *   - Admission control prevents overload (total utilisation <= 1 per CPU).
 *   - When a task exhausts its budget before its deadline, it is throttled
 *     until the next period, preventing interference with other RT tasks.
 *
 * Integration with the existing scheduler:
 *   - SCHED_DEADLINE tasks are tracked in a per-CPU deadline runqueue.
 *   - schedule() checks deadline tasks first via sched_deadline_pick_next().
 *   - scheduler_tick() calls sched_deadline_tick() for deadline tasks.
 *   - A periodic replenishment check (sched_deadline_replenish()) is called
 *     from the scheduler tick handler.
 */

#include "sched_deadline.h"
#include "timer.h"
#include "smp.h"
#include "string.h"
#include "printf.h"
#include "process.h"
#include "scheduler.h"

/* ── Per-CPU deadline runqueues ──────────────────────────────────────── */

static struct cpu_dl_rq cpu_dl_rq[SMP_MAX_CPUS];

/* ── Initialisation ──────────────────────────────────────────────────── */

void sched_deadline_init_cpu(int cpu)
{
    if (cpu < 0 || cpu >= SMP_MAX_CPUS)
        return;

    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu];
    memset(dl_rq, 0, sizeof(*dl_rq));
}

/* ── Utilisation helpers ─────────────────────────────────────────────── */

/*
 * Compute (runtime << BW_SHIFT) / period as a fixed-point value.
 * Clamps to DL_BW_UNIT (i.e., 1.0 in fixed point) to prevent overflow.
 */
static uint64_t dl_bw(uint64_t runtime, uint64_t period)
{
    if (period == 0 || runtime == 0)
        return 0;

    /* Shift up for precision, then divide */
    uint64_t bw = (runtime << DL_BW_SHIFT) / period;

    /* Clamp to 1.0 */
    if (bw > DL_BW_UNIT)
        bw = DL_BW_UNIT;

    return bw;
}

/*
 * Validate deadline scheduling parameters for a process.
 * Returns 0 on success, -1 (EINVAL) on invalid params.
 */
static int dl_params_valid(const struct process *proc)
{
    if (!proc)
        return -1;

    /* All parameters must be positive */
    if (proc->dl_runtime == 0 || proc->dl_deadline == 0 || proc->dl_period == 0)
        return -1;

    /* Runtime must not exceed deadline */
    if (proc->dl_runtime > proc->dl_deadline)
        return -1;

    /* Deadline must not exceed period (constrained deadline) */
    if (proc->dl_deadline > proc->dl_period)
        return -1;

    return 0;
}

/* ── Admission control ───────────────────────────────────────────────── */

/*
 * Check whether adding a task with the given bandwidth would exceed the
 * CPU capacity.  Returns 0 if admitted, -EBUSY if capacity exceeded.
 */
static int dl_admission_control(int cpu, uint64_t bw)
{
    if (cpu < 0 || cpu >= smp_cpu_count)
        return -1;

    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu];

    /* Check max task count */
    if (dl_rq->nr_tasks >= SCHED_DL_MAX_PER_CPU)
        return -1;

    /* Check total utilisation <= 1.0 */
    if (dl_rq->total_bw + bw > DL_BW_UNIT)
        return -1;

    return 0;
}

/* ── Add / Remove tasks ──────────────────────────────────────────────── */

int sched_deadline_add_task(struct process *proc)
{
    if (!proc)
        return -1;

    /* Validate parameters */
    if (dl_params_valid(proc) != 0)
        return -1;

    /* Determine target CPU (use process affinity, or current CPU) */
    uint32_t cpu_id = get_cpu_id();
    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];

    /* Admission control */
    uint64_t bw = dl_bw(proc->dl_runtime, proc->dl_period);
    if (dl_admission_control(cpu_id, bw) != 0)
        return -1;

    /* Mark as active and initialise deadline state for first period */
    uint64_t now_ns = timer_get_ns();
    proc->dl_active = 1;
    proc->dl_period_start = now_ns;
    proc->dl_deadline_abs = now_ns + proc->dl_deadline;
    proc->dl_runtime_remaining = proc->dl_runtime;
    proc->dl_consumed = 0;
    proc->dl_throttled = 0;

    /* Add to per-CPU array */
    dl_rq->tasks[dl_rq->nr_tasks++] = proc;
    dl_rq->total_bw += bw;

    return 0;
}

void sched_deadline_remove_task(struct process *proc)
{
    if (!proc || !proc->dl_active)
        return;

    uint32_t cpu_id = get_cpu_id();
    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];

    /* Find and remove from the array */
    int found = 0;
    for (int i = 0; i < dl_rq->nr_tasks; i++) {
        if (dl_rq->tasks[i] == proc) {
            found = 1;
            /* Reclaim any unused budget before removing */
            if (proc->dl_consumed < proc->dl_runtime) {
                uint64_t unused_ns = proc->dl_runtime - proc->dl_consumed;
                uint64_t unused_bw = (unused_ns << DL_BW_SHIFT) / proc->dl_period;
                if (unused_bw > DL_BW_UNIT) unused_bw = DL_BW_UNIT;
                uint64_t max_reclaim = dl_rq->total_bw * 2;
                if (dl_rq->reclaimed_bw + unused_bw > max_reclaim)
                    dl_rq->reclaimed_bw = max_reclaim;
                else
                    dl_rq->reclaimed_bw += unused_bw;
            }
            dl_rq->total_bw -= dl_bw(proc->dl_runtime, proc->dl_period);
            for (int j = i; j < dl_rq->nr_tasks - 1; j++)
                dl_rq->tasks[j] = dl_rq->tasks[j + 1];
            dl_rq->nr_tasks--;
            proc->dl_active = 0;
            break;
        }
    }

    if (!found) {
        /* Maybe on a different CPU — scan all CPUs */
        for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
            struct cpu_dl_rq *rq = &cpu_dl_rq[cpu];
            for (int i = 0; i < rq->nr_tasks; i++) {
                if (rq->tasks[i] == proc) {
                    /* Reclaim any unused budget before removing */
                    if (proc->dl_consumed < proc->dl_runtime) {
                        uint64_t unused_ns = proc->dl_runtime - proc->dl_consumed;
                        uint64_t unused_bw = (unused_ns << DL_BW_SHIFT) / proc->dl_period;
                        if (unused_bw > DL_BW_UNIT) unused_bw = DL_BW_UNIT;
                        uint64_t max_reclaim = rq->total_bw * 2;
                        if (rq->reclaimed_bw + unused_bw > max_reclaim)
                            rq->reclaimed_bw = max_reclaim;
                        else
                            rq->reclaimed_bw += unused_bw;
                    }
                    rq->total_bw -= dl_bw(proc->dl_runtime, proc->dl_period);
                    for (int j = i; j < rq->nr_tasks - 1; j++)
                        rq->tasks[j] = rq->tasks[j + 1];
                    rq->nr_tasks--;
                    break;
                }
            }
        }
        proc->dl_active = 0;
    }
}

/* ── EDF scheduler — pick next deadline task with GRUB reclaim ────── */

struct process *sched_deadline_pick_next(void)
{
    uint32_t cpu_id = get_cpu_id();
    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];

    if (dl_rq->nr_tasks == 0)
        return NULL;

    struct process *best = NULL;
    uint64_t earliest_deadline = UINT64_MAX;
    int has_reclaim = (dl_rq->reclaimed_bw > 0);

    for (int i = 0; i < dl_rq->nr_tasks; i++) {
        struct process *proc = dl_rq->tasks[i];
        if (!proc || !proc->dl_active)
            continue;

        /* Skip non-runnable tasks */
        if (proc->state != PROCESS_READY && proc->state != PROCESS_RUNNING)
            continue;

        /* Skip throttled tasks unless reclaim bandwidth is available */
        if (proc->dl_throttled) {
            if (!has_reclaim)
                continue;
            /* Even with reclaim, only pick if there's actually reclaim bw */
            if (dl_rq->reclaimed_bw == 0)
                continue;
        } else if (proc->dl_runtime_remaining == 0) {
            /* Budget exhausted but not throttled (yet) — skip */
            continue;
        }

        /* Pick the task with the earliest absolute deadline */
        if (proc->dl_deadline_abs < earliest_deadline) {
            earliest_deadline = proc->dl_deadline_abs;
            best = proc;
        }
    }

    return best;
}

/* ── Tick handler — budget accounting with GRUB reclaim ────────────── */

void sched_deadline_tick(struct process *proc)
{
    if (!proc || !proc->dl_active)
        return;

    /* Each tick consumes one tick's worth of runtime */
    uint64_t tick_ns = NS_PER_TICK;

    /* Track actual consumption for GRUB reclaim accounting */
    proc->dl_consumed += tick_ns;

    /* Try to consume from own budget first */
    if (proc->dl_runtime_remaining > tick_ns) {
        proc->dl_runtime_remaining -= tick_ns;
        return;
    }

    if (proc->dl_runtime_remaining > 0) {
        /* Partially consume remaining own budget */
        uint64_t remainder = tick_ns - proc->dl_runtime_remaining;
        proc->dl_runtime_remaining = 0;

        /* If remainder > 0, try reclaim pool */
        if (remainder > 0) {
            uint32_t cpu_id = get_cpu_id();
            struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];
            if (dl_rq->reclaimed_bw > 0) {
                /* Consume from reclaim pool — convert ns to fixed-point bw */
                uint64_t consumed_bw = (remainder << DL_BW_SHIFT) / proc->dl_period;
                if (consumed_bw > dl_rq->reclaimed_bw)
                    consumed_bw = dl_rq->reclaimed_bw;
                dl_rq->reclaimed_bw -= consumed_bw;
                return;  /* still running on reclaimed bandwidth */
            }
        }
    } else {
        /* Own budget already exhausted — try reclaim pool */
        uint32_t cpu_id = get_cpu_id();
        struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];
        if (dl_rq->reclaimed_bw > 0) {
            uint64_t consumed_bw = (tick_ns << DL_BW_SHIFT) / proc->dl_period;
            if (consumed_bw > dl_rq->reclaimed_bw)
                consumed_bw = dl_rq->reclaimed_bw;
            dl_rq->reclaimed_bw -= consumed_bw;
            return;  /* running on reclaimed bandwidth */
        }
    }

    /* Budget exhausted and no reclaim available — throttle */
    uint64_t now_ns = timer_get_ns();
    if (now_ns < proc->dl_deadline_abs) {
        proc->dl_throttled = 1;
    }
}

/* ── GRUB reclaim: task blocked before budget exhaustion ──────────── */

void sched_deadline_task_blocked(struct process *proc)
{
    if (!proc || !proc->dl_active || proc->dl_throttled)
        return;

    /* Calculate unused budget for this period.
     * dl_consumed is what we actually used; dl_runtime is what was allocated.
     * If consumed < allocated, the difference is reclaimable bandwidth. */
    if (proc->dl_consumed < proc->dl_runtime) {
        uint64_t unused_ns = proc->dl_runtime - proc->dl_consumed;

        /* Convert unused runtime (ns) to fixed-point bandwidth and add to
         * the per-CPU reclaim pool.  Clamp to prevent arithmetic overflow. */
        uint64_t unused_bw = (unused_ns << DL_BW_SHIFT) / proc->dl_period;
        if (unused_bw > DL_BW_UNIT)
            unused_bw = DL_BW_UNIT;

        uint32_t cpu_id = get_cpu_id();
        struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];

        /* Cap reclaimed_bw to prevent unbounded accumulation (max 2x total_bw) */
        uint64_t max_reclaim = dl_rq->total_bw * 2;
        if (dl_rq->reclaimed_bw + unused_bw > max_reclaim)
            dl_rq->reclaimed_bw = max_reclaim;
        else
            dl_rq->reclaimed_bw += unused_bw;
    }
}

/* ── Replenishment — with GRUB unused-bandwidth harvesting ──────────── */

void sched_deadline_replenish(void)
{
    uint32_t cpu_id = get_cpu_id();
    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];
    uint64_t now_ns = timer_get_ns();

    for (int i = 0; i < dl_rq->nr_tasks; i++) {
        struct process *proc = dl_rq->tasks[i];
        if (!proc || !proc->dl_active)
            continue;

        /* Check if we've passed the absolute deadline (end of current period) */
        if (now_ns < proc->dl_deadline_abs)
            continue;

        /* Before replenishing, harvest any unused bandwidth from the
         * just-completed period and add it to the GRUB reclaim pool. */
        if (proc->dl_consumed < proc->dl_runtime) {
            uint64_t unused_ns = proc->dl_runtime - proc->dl_consumed;
            uint64_t unused_bw = (unused_ns << DL_BW_SHIFT) / proc->dl_period;
            if (unused_bw > DL_BW_UNIT)
                unused_bw = DL_BW_UNIT;

            uint64_t max_reclaim = dl_rq->total_bw * 2;
            if (dl_rq->reclaimed_bw + unused_bw > max_reclaim)
                dl_rq->reclaimed_bw = max_reclaim;
            else
                dl_rq->reclaimed_bw += unused_bw;
        }

        /* Advance one or more periods until deadline_abs is in the future */
        while (now_ns >= proc->dl_deadline_abs) {
            proc->dl_period_start = proc->dl_deadline_abs;
            proc->dl_deadline_abs += proc->dl_period;
        }

        /* Replenish budget for the new period and reset consumption tracking */
        proc->dl_runtime_remaining = proc->dl_runtime;
        proc->dl_consumed = 0;
        proc->dl_throttled = 0;

        /* If the task was blocked, wake it up and add to runqueue */
        if (proc->state == PROCESS_BLOCKED) {
            proc->state = PROCESS_READY;
        }
        if (proc->state == PROCESS_READY && !proc->on_queue) {
            scheduler_add(proc);
        }
    }
}

/* ── Runnable check — includes GRUB reclaim eligibility ────────────── */

int sched_deadline_is_runnable(struct process *proc)
{
    if (!proc || !proc->dl_active)
        return 0;

    /* Must be in READY or RUNNING state */
    if (proc->state != PROCESS_READY && proc->state != PROCESS_RUNNING)
        return 0;

    /* Not throttled — can run on own budget */
    if (!proc->dl_throttled && proc->dl_runtime_remaining > 0)
        return 1;

    /* Throttled but reclaim bandwidth available — still runnable */
    if (proc->dl_throttled) {
        uint32_t cpu_id = get_cpu_id();
        struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu_id];
        if (dl_rq->reclaimed_bw > 0)
            return 1;
    }

    return 0;
}

/* ── Update deadline ─────────────────────────────────────────────────── */

void sched_deadline_update_deadline(struct process *proc)
{
    if (!proc || !proc->dl_active)
        return;

    uint64_t now_ns = timer_get_ns();
    proc->dl_period_start = now_ns;
    proc->dl_deadline_abs = now_ns + proc->dl_deadline;
    proc->dl_runtime_remaining = proc->dl_runtime;
    proc->dl_consumed = 0;
    proc->dl_throttled = 0;
}

/* ── Debug dump ──────────────────────────────────────────────────────── */

void sched_deadline_dump(int cpu)
{
    if (cpu < 0 || cpu >= smp_cpu_count) {
        kprintf("sched_deadline: invalid CPU %d\n", cpu);
        return;
    }

    struct cpu_dl_rq *dl_rq = &cpu_dl_rq[cpu];
    kprintf("CPU %d deadline tasks: %d (total_bw=%llu/%llu, reclaimed=%llu)\n",
           cpu, dl_rq->nr_tasks,
           (unsigned long long)dl_rq->total_bw,
           (unsigned long long)DL_BW_UNIT,
           (unsigned long long)dl_rq->reclaimed_bw);

    for (int i = 0; i < dl_rq->nr_tasks; i++) {
        struct process *proc = dl_rq->tasks[i];
        if (!proc) continue;
        kprintf("  [%d] pid=%u name=%s runtime=%llu deadline=%llu period=%llu "
               "dl_abs=%llu remaining=%llu consumed=%llu throttled=%d\n",
               i, proc->pid, proc->name ? proc->name : "?",
               (unsigned long long)proc->dl_runtime,
               (unsigned long long)proc->dl_deadline,
               (unsigned long long)proc->dl_period,
               (unsigned long long)proc->dl_deadline_abs,
               (unsigned long long)proc->dl_runtime_remaining,
               (unsigned long long)proc->dl_consumed,
               proc->dl_throttled);
    }
}

/* ── Stub: deadline_task_new ─────────────────────────────────── */
int deadline_task_new(struct process *proc)
{
    (void)proc;
    kprintf("[sched_deadline] deadline_task_new: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: deadline_task_delta ───────────────────────────────── */
int deadline_task_delta(struct process *proc, int delta)
{
    (void)proc;
    (void)delta;
    kprintf("[sched_deadline] deadline_task_delta: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: deadline_push_task ────────────────────────────────── */
int deadline_push_task(struct process *proc)
{
    (void)proc;
    kprintf("[sched_deadline] deadline_push_task: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: deadline_pull_task ────────────────────────────────── */
struct process *deadline_pull_task(int cpu)
{
    (void)cpu;
    kprintf("[sched_deadline] deadline_pull_task: not yet implemented\n");
    return NULL;
}

/* ── Stub: deadline_task_tick ────────────────────────────────── */
void deadline_task_tick(struct process *proc)
{
    (void)proc;
    kprintf("[sched_deadline] deadline_task_tick: not yet implemented\n");
}
