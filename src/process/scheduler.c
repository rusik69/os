/*
 * Per-CPU scheduler with SMP load balancing
 *
 * Each CPU maintains its own multilevel priority queue (4 levels).
 * Idle CPUs pull tasks from busy CPUs via load_balance().
 * Cross-CPU wakeups use IPI reschedule.
 */
#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "syscall.h"
#include "io.h"
#include "gdt.h"
#include "vmm.h"
#include "timer.h"
#include "smp.h"
#include "spinlock.h"
#include "apic.h"
#include "string.h"
#include "rtc.h"
#include "nmi_watchdog.h"
#include "cpuhp.h"
#include "cpuidle.h"

/* 4-level multilevel priority queue: 0 = highest, 3 = lowest */

/* Global lock for cross-CPU operations (load balancing, process table walks) */
static spinlock_t sched_lock = SPINLOCK_INIT;

extern uint64_t syscall_kernel_rsp;

/* Time slices in ticks (100Hz): higher priority = larger quantum. */
static const uint16_t time_slices[SCHED_LEVELS] = {10, 5, 3, 2};

/* CFS constants */
#define CFS_NICE_0_WEIGHT 1024
#define CFS_WEIGHT_SHIFT 10  /* 1024 = 1<<10 */
#define CFS_VRUNTIME_MAX_DIFF 100000000ULL /* 100ms */

/* ── Autogroup state ──────────────────────────────────────── */
static struct sched_autogroup autogroups[SCHED_AUTOGROUP_MAX];
/* autogroup_count tracks total groups — kept for future use */
static int autogroup_count __attribute__((unused)) = 0;

/* ── Per-CPU helpers ────────────────────────────────────────────────── */
static inline struct cpu_info *this_cpu(void) {
    return get_cpu_info();
}

static inline int cpu_queue_empty(struct cpu_info *ci, int lvl) {
    return ci->queue_head[lvl] == NULL;
}

static inline int cpu_queues_empty(struct cpu_info *ci) {
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        if (ci->queue_head[lvl]) return 0;
    }
    return 1;
}

/* Count runnable tasks on a given CPU (approximate, no lock needed for stats) */
static int cpu_nr_runnable(struct cpu_info *ci) {
    int count = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) { count++; p = p->next; }
    }
    return count;
}

/* ── Initialization ─────────────────────────────────────────────────── */
void scheduler_init(void) {
    /* Per-CPU queues are zero-initialized by smp_init_bsp.
     * This function ensures the BSP's scheduler is marked enabled. */
    struct cpu_info *ci = this_cpu();
    ci->scheduler_enabled = 1;

    /* Sync the per-CPU current_process pointer.
     * process_init() set the global 'current_process' to &process_table[0]
     * (the boot/idle process), but smp_init_bsp() then cleared the
     * per-CPU copy to NULL.  Without this, scheduler_tick() sees NULL
     * via ci->current_process and returns immediately — no quantum is
     * assigned and schedule() is never called, so test tasks (and any
     * other process) never get to run. */
    if (!ci->current_process) {
        ci->current_process = process_get_current();
    }
}

/* ── Add process to its CPU's runqueue ──────────────────────────────── */
void scheduler_add(struct process *proc) {
    if (proc->on_queue) return;

    /* Determine target CPU: use cpu_affinity hint, or current CPU */
    uint32_t cpu_id = get_cpu_id();
    struct cpu_info *ci = &cpu_info_array[cpu_id];

    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;

    proc->next = NULL;
    proc->on_queue = 1;

    /* Lock the per-CPU queue */
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    if (!ci->queue_tail[lvl]) {
        ci->queue_head[lvl] = proc;
        ci->queue_tail[lvl] = proc;
    } else {
        ci->queue_tail[lvl]->next = proc;
        ci->queue_tail[lvl] = proc;
    }

    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Remove process from its queue ──────────────────────────────────── */
void scheduler_remove(struct process *proc) {
    if (!proc->on_queue) return;

    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    /* Search all CPUs for this process (it could be on any queue) */
    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        struct cpu_info *ci = &cpu_info_array[cpu];
        struct process *prev = NULL;
        struct process *cur = ci->queue_head[lvl];

        while (cur) {
            if (cur == proc) {
                if (prev) prev->next = cur->next;
                else ci->queue_head[lvl] = cur->next;
                if (cur == ci->queue_tail[lvl])
                    ci->queue_tail[lvl] = prev;
                cur->next = NULL;
                proc->on_queue = 0;
                spinlock_irqsave_release(&sched_lock, irq_flags);
                return;
            }
            prev = cur;
            cur = cur->next;
        }
    }

    proc->on_queue = 0;
    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Priority change ────────────────────────────────────────────────── */
int scheduler_set_priority(struct process *proc, uint8_t priority) {
    if (!proc || priority >= SCHED_LEVELS) return -1;

    if (proc->state == PROCESS_READY) {
        scheduler_remove(proc);
        proc->priority = priority;
        scheduler_add(proc);
        return 0;
    }

    proc->priority = priority;
    return 0;
}

/* ── Dequeue next runnable process from current CPU ─────────────────── */
static struct process *dequeue_next(void) {
    struct cpu_info *ci = this_cpu();

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        if (ci->queue_head[lvl]) {
            struct process *p = ci->queue_head[lvl];
            ci->queue_head[lvl] = p->next;
            if (!ci->queue_head[lvl]) ci->queue_tail[lvl] = NULL;
            p->next = NULL;
            p->on_queue = 0;
            return p;
        }
    }
    return NULL;
}

/* ── Load balancing: steal from the busiest CPU (weighted) ──── */
static int calculate_cpu_load(struct cpu_info *ci) {
    int load = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) {
            load += (int)(p->sched_weight ? p->sched_weight : CFS_NICE_0_WEIGHT);
            p = p->next;
        }
    }
    return load;
}

static int load_balance(void) {
    struct cpu_info *ci = this_cpu();
    int this_load = calculate_cpu_load(ci);

    /* Don't steal if we already have significant load */
    if (this_load > CFS_NICE_0_WEIGHT * 2) return 0;

    /* Find the busiest CPU with a significant load imbalance */
    int busiest_cpu = -1;
    int busiest_load = 0;

    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        if ((uint32_t)cpu == get_cpu_id()) continue;
        struct cpu_info *other = &cpu_info_array[cpu];
        int load = calculate_cpu_load(other);
        if (load > busiest_load && load - this_load > CFS_NICE_0_WEIGHT) {
            busiest_load = load;
            busiest_cpu = cpu;
        }
    }

    if (busiest_cpu < 0) return 0;

    /* Steal the lowest-priority task from the busiest CPU */
    struct cpu_info *busy = &cpu_info_array[busiest_cpu];

    for (int lvl = SCHED_LEVELS - 1; lvl >= 0; lvl--) {
        if (!busy->queue_head[lvl]) continue;

        /* Find the last (tail) process in this priority level */
        struct process *prev = NULL;
        struct process *cur = busy->queue_head[lvl];
        while (cur->next) {
            prev = cur;
            cur = cur->next;
        }

        /* Unlink from busy CPU */
        if (prev) prev->next = NULL;
        else busy->queue_head[lvl] = NULL;
        busy->queue_tail[lvl] = prev;

        cur->next = NULL;
        cur->on_queue = 0;

        /* Add to our queue */
        {
            int our_lvl = (int)cur->priority;
            if (!ci->queue_tail[our_lvl]) {
                ci->queue_head[our_lvl] = cur;
                ci->queue_tail[our_lvl] = cur;
            } else {
                ci->queue_tail[our_lvl]->next = cur;
                ci->queue_tail[our_lvl] = cur;
            }
            cur->on_queue = 1;
        }

        return 1; /* stole one task */
    }

    return 0;
}

/* Forward declaration — defined below (line ~552), called from scheduler_tick */
static void update_vruntime(struct process *p, int ticks);

/* ── Main scheduler entry: pick next process, context-switch ────────── */
void schedule(void) {
    struct cpu_info *ci = this_cpu();
    if (!ci->scheduler_enabled) return;

    __asm__ volatile("cli");

    struct process *current = ci->current_process;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);
    struct process *next = dequeue_next();
    spinlock_irqsave_release(&sched_lock, irq_flags);

    if (!next) {
        /* No tasks — try load balancing */
        spinlock_irqsave_acquire(&sched_lock, &irq_flags);
        int stole = load_balance();
        spinlock_irqsave_release(&sched_lock, irq_flags);

        if (stole) {
            spinlock_irqsave_acquire(&sched_lock, &irq_flags);
            next = dequeue_next();
            spinlock_irqsave_release(&sched_lock, irq_flags);
        }

        if (!next) {
            ci->idle_ticks++;
            /* Use cpuidle to enter an appropriate C-state.
             * cpuidle_idle() handles watchdog petting internally. */
            cpuidle_idle();
            __asm__ volatile("sti");
            return;
        }
    }

    /* Put current back on queue if still runnable */
    if (current && current->state == PROCESS_RUNNING) {
        current->nivcsw++;  /* preempted — involuntary context switch */
        current->state = PROCESS_READY;
        spinlock_irqsave_acquire(&sched_lock, &irq_flags);
        scheduler_add(current);
        spinlock_irqsave_release(&sched_lock, irq_flags);
    } else if (current) {
        current->nvcsw++;   /* yielded or blocked — voluntary context switch */
    }

    next->state = PROCESS_RUNNING;
    next->ticks_remaining = time_slices[(int)next->priority];
    next->last_run_tick = timer_get_ticks();
    process_set_current(next);

    /* Set kernel stack for syscall/interrupt returns */
    tss_set_rsp0(next->stack_top);
    syscall_kernel_rsp = next->stack_top;

    /* Switch page tables if user process */
    if (next->is_user && next->pml4) {
        vmm_switch_pml4(next->pml4);
    } else {
        vmm_switch_pml4(vmm_get_pml4());
    }

    /* Pet the watchdog: we just context-switched, proving the scheduler
     * is making forward progress on this CPU. */
    nmi_watchdog_pet();

    context_switch(current ? &current->context : NULL, next->context);
    __asm__ volatile("sti");
}

void scheduler_yield(void) {
    /* Priority boost: voluntarily yielding processes get a temporary
     * priority bump to discourage busy-waiting and improve interactivity */
    struct process *cur = process_get_current();
    if (cur && cur->priority > 0) {
        cur->priority--; /* boost by one level */
    }
    schedule();
}

uint64_t scheduler_get_idle_ticks(void) {
    return this_cpu()->idle_ticks;
}

/* ── Timer tick handler ─────────────────────────────────────────────── */
void scheduler_tick(int was_user) {
    struct cpu_info *ci = this_cpu();
    if (!ci->scheduler_enabled) return;

    /* Pet the soft watchdog — this proves timer IRQs are reaching the
     * scheduler, which distinguishes soft lockups (scheduler stuck) from
     * hard lockups (IRQs disabled entirely). */
    nmi_watchdog_soft_pet();

    /* Check for soft lockup (scheduler not invoked despite timer ticks) */
    nmi_watchdog_check_soft();

    struct process *cur = ci->current_process;
    if (!cur || cur->state != PROCESS_RUNNING) return;

    /* Account CPU time: user time if we interrupted user code,
     * system time if we interrupted kernel code. */
    if (was_user && cur->is_user) {
        cur->utime_ticks++;
        cur->cpu_user++;
    } else {
        cur->stime_ticks++;
        cur->cpu_system++;
    }

    /* Update CFS vruntime */
    update_vruntime(cur, 1);

    /* Check pending signals */
    if (cur->pending_signals) {
        signal_check();
        cur = ci->current_process;
        if (!cur || cur->state != PROCESS_RUNNING) {
            schedule();
            return;
        }
    }

    /* First tick: assign quantum without preempting */
    if (cur->ticks_remaining == 0) {
        if (cur->sched_policy == SCHED_OTHER) {
            int lvl = (int)cur->priority;
            if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
            cur->ticks_remaining = time_slices[lvl];
        } else if (cur->sched_policy == SCHED_BATCH) {
            /* SCHED_BATCH: longer timeslices, lower priority (level 3) */
            cur->ticks_remaining = time_slices[3] * 3;
        } else {
            /* SCHED_FIFO / SCHED_RR: use maximum quantum for priority level */
            int lvl = (int)cur->priority;
            if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
            cur->ticks_remaining = time_slices[lvl] * 2;
        }
        return;
    }

    cur->ticks_remaining--;

    if (cur->ticks_remaining == 0) {
        if (cur->sched_policy == SCHED_FIFO) {
            /* SCHED_FIFO: replenish quantum, don't preempt */
            int lvl = (int)cur->priority;
            if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
            cur->ticks_remaining = time_slices[lvl];
        } else if (cur->sched_policy == SCHED_RR) {
            /* SCHED_RR: rotate to end of queue on slice expiry */
            int lvl = (int)cur->priority;
            if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
            cur->ticks_remaining = time_slices[lvl];
            schedule();
        } else {
            /* SCHED_OTHER: preempt on quantum expiry */
            schedule();
        }
    }
}

/* ── Aging: boost starved processes (using RTC for precision) ──── */
void scheduler_age(void) {
    extern uint64_t rtc_get_ticks(void);
    uint64_t now = rtc_get_ticks();
    struct process *table = process_get_table();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &table[i];
        if (p->state != PROCESS_READY) continue;
        if (p->priority == 0) continue;
        if (p->last_run_tick == 0) continue;
        uint64_t age_threshold = 1000;
        if (now - p->last_run_tick > age_threshold) {
            /* Remove, boost, re-add */
            scheduler_remove(p);
            p->priority--;
            p->last_run_tick = now;
            scheduler_add(p);
        }
    }

    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Wake blocked processes whose sleep timer expired ───────────────── */
void scheduler_wake_sleepers(void) {
    uint64_t now = timer_get_ticks();
    struct process *table = process_get_table();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_BLOCKED && table[i].sleep_until > 0 &&
            now >= table[i].sleep_until) {
            table[i].sleep_until = 0;
            table[i].state = PROCESS_READY;

            /* Add to the CPU that last ran this process (affinity-based) */
            scheduler_add(&table[i]);
        }
    }

    spinlock_irqsave_release(&sched_lock, irq_flags);
}

/* ── Scheduler statistics ────────────────────────────────────── */

static struct sched_stats sched_stats_data;

void scheduler_stats_inc_ctx_switch(void) { sched_stats_data.context_switches++; }
void scheduler_stats_inc_preempt(void)    { sched_stats_data.preemptions++; }
void scheduler_stats_inc_yield(void)      { sched_stats_data.yields++; }

void scheduler_get_stats(struct sched_stats *stats) {
    if (!stats) return;
    stats->context_switches = sched_stats_data.context_switches;
    stats->preemptions = sched_stats_data.preemptions;
    stats->yields = sched_stats_data.yields;
    stats->idle_ticks_total = sched_stats_data.idle_ticks_total;
}

/* ── Per-CPU runqueue statistics ─────────────────────────────── */

void scheduler_get_runqueue_stats(int cpu, struct runqueue_stats *s) {
    if (!s || cpu < 0 || cpu >= smp_cpu_count) return;
    struct cpu_info *ci = &cpu_info_array[cpu];
    memset(s, 0, sizeof(*s));

    int total_load = 0;
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];
        while (p) {
            s->nr_runnable++;
            s->prio_distribution[lvl]++;
            /* Load weight: each runnable process contributes (4 - priority) */
            total_load += (SCHED_LEVELS - lvl);
            p = p->next;
        }
    }
    s->load_weight = total_load;

    /* Count processes in various states from the process table */
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_RUNNING) s->nr_running++;
        if (table[i].state == PROCESS_BLOCKED) s->nr_uninterruptible++;
    }
}

/* ── Autogroup implementation ───────────────────────────────── */

int sched_autogroup_get(int session_id) {
    /* Session ID maps to autogroup index via simple hash */
    int target = session_id % SCHED_AUTOGROUP_MAX;
    if (autogroups[target].id == session_id) return target;

    /* Find existing group */
    for (int i = 0; i < SCHED_AUTOGROUP_MAX; i++) {
        if (autogroups[i].id == session_id) return i;
    }

    /* Create new group */
    for (int i = 0; i < SCHED_AUTOGROUP_MAX; i++) {
        if (autogroups[i].id == 0 || autogroups[i].member_count == 0) {
            autogroups[i].id = session_id;
            autogroups[i].vruntime = 0;
            autogroups[i].member_count = 0;
            return i;
        }
    }
    return -1;
}

void sched_autogroup_assign(struct process *proc, int group_id) {
    if (group_id < 0 || group_id >= SCHED_AUTOGROUP_MAX) return;
    if (proc->sched_autogroup_id >= 0 && proc->sched_autogroup_id < SCHED_AUTOGROUP_MAX) {
        if (autogroups[proc->sched_autogroup_id].member_count > 0)
            autogroups[proc->sched_autogroup_id].member_count--;
    }
    proc->sched_autogroup_id = group_id;
    if (group_id >= 0)
        autogroups[group_id].member_count++;
}

uint64_t sched_autogroup_max_vruntime(int group_id) {
    if (group_id < 0 || group_id >= SCHED_AUTOGROUP_MAX) return 0;
    uint64_t max = 0;
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED &&
            table[i].sched_autogroup_id == group_id &&
            table[i].vruntime > max)
            max = table[i].vruntime;
    }
    return max;
}

/* ── Update vruntime on each tick (CFS) ─────────────────────── */
void update_vruntime(struct process *p, int ticks) {
    if (!p) return;
    uint64_t weight = p->sched_weight ? p->sched_weight : CFS_NICE_0_WEIGHT;
    /* vruntime += time_slice * 1000000 / weight */
    uint64_t increment = (uint64_t)ticks * 1000000ULL * CFS_NICE_0_WEIGHT / weight;
    p->vruntime += increment;
}

/* ── Dequeue next process based on lowest vruntime (CFS) ──── */
static struct process *dequeue_next_cfs(void) {
    struct cpu_info *ci = this_cpu();
    struct process *best = NULL;
    int best_lvl = -1;
    struct process *best_prev = NULL;

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *prev = NULL;
        struct process *cur = ci->queue_head[lvl];
        while (cur) {
            if (!best || cur->vruntime < best->vruntime) {
                best = cur;
                best_lvl = lvl;
                best_prev = prev;
            }
            prev = cur;
            cur = cur->next;
        }
    }

    if (!best) return NULL;

    /* Unlink best from its queue */
    if (best_prev)
        best_prev->next = best->next;
    else
        ci->queue_head[best_lvl] = best->next;

    if (best == ci->queue_tail[best_lvl])
        ci->queue_tail[best_lvl] = best_prev;

    best->next = NULL;
    best->on_queue = 0;
    return best;
}

/* ── Replace old dequeue_next with CFS version ────────────── */
#define dequeue_next dequeue_next_cfs

/* ── CPU hotplug: migrate all tasks away from a CPU ──────── */

/*
 * Migrate every runnable process from @from_cpu to other online CPUs.
 * This is called by cpuhp_migrate_tasks_away() while holding the hotplug
 * lock. It iterates the per-CPU runqueue of @from_cpu, removes each
 * process, and distributes them across the remaining online CPUs.
 *
 * Returns the number of tasks migrated (0 if none).
 *
 * NOTE: This function expects cpuhp_lock to already be held by the caller
 * and interrupts to be disabled. It does NOT acquire sched_lock itself
 * since the hotplug lock serialises all scheduling modifications during
 * the offline transition.
 */
int scheduler_migrate_tasks_from(int from_cpu)
{
    struct cpu_info *ci;
    int migrated = 0;

    if (from_cpu < 0 || from_cpu >= smp_cpu_count)
        return -1;

    ci = &cpu_info_array[from_cpu];

    /* ── Count available destination CPUs ────────────────────────── */
    int dst_cpus[SMP_MAX_CPUS];
    int num_dst = 0;

    for (int i = 0; i < smp_cpu_count; i++) {
        if (i != from_cpu && cpuhp_is_online(i)) {
            dst_cpus[num_dst++] = i;
        }
    }

    if (num_dst == 0)
        return 0; /* no targets — nothing to do */

    /* ── Migrate tasks from each priority level ──────────────────── */
    int spread = (int)timer_get_ticks(); /* pseudo-random spread seed */

    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        struct process *p = ci->queue_head[lvl];

        while (p) {
            struct process *next = p->next;

            /* Pick a destination CPU (round-robin) */
            int dst = dst_cpus[spread % num_dst];
            spread++;

            /* Unlink from source queue */
            p->next = NULL;
            p->on_queue = 0;

            /* Link into destination queue */
            struct cpu_info *dci = &cpu_info_array[dst];
            int dlvl = (int)p->priority;
            if (dlvl < 0 || dlvl >= SCHED_LEVELS)
                dlvl = 1;

            if (!dci->queue_tail[dlvl]) {
                dci->queue_head[dlvl] = p;
                dci->queue_tail[dlvl] = p;
            } else {
                dci->queue_tail[dlvl]->next = p;
                dci->queue_tail[dlvl] = p;
            }
            p->on_queue = 1;
            migrated++;

            p = next;
        }

        /* Clear the source queue for this level */
        ci->queue_head[lvl] = NULL;
        ci->queue_tail[lvl] = NULL;
    }

    return migrated;
}
