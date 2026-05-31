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

/* 4-level multilevel priority queue: 0 = highest, 3 = lowest */

/* Global lock for cross-CPU operations (load balancing, process table walks) */
static spinlock_t sched_lock = SPINLOCK_INIT;

extern uint64_t syscall_kernel_rsp;

/* Time slices in ticks (100Hz): higher priority = larger quantum. */
static const uint16_t time_slices[SCHED_LEVELS] = {10, 5, 3, 2};

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

/* ── Load balancing: steal from the busiest CPU ─────────────────────── */
static int load_balance(void) {
    struct cpu_info *ci = this_cpu();
    int this_nr = cpu_nr_runnable(ci);

    /* Don't steal if we already have tasks */
    if (this_nr > 1) return 0;

    /* Find the busiest CPU with a significant load imbalance */
    int busiest_cpu = -1;
    int busiest_nr = 0;

    for (int cpu = 0; cpu < smp_cpu_count; cpu++) {
        if ((uint32_t)cpu == get_cpu_id()) continue;
        struct cpu_info *other = &cpu_info_array[cpu];
        int nr = cpu_nr_runnable(other);
        /* Steal if imbalance > 1 (i.e., other has at least 2 more than us) */
        if (nr > busiest_nr && nr - this_nr >= 2) {
            busiest_nr = nr;
            busiest_cpu = cpu;
        }
    }

    if (busiest_cpu < 0) return 0;

    /* Steal the lowest-priority task from the busiest CPU */
    struct cpu_info *busy = &cpu_info_array[busiest_cpu];

    for (int lvl = SCHED_LEVELS - 1; lvl >= 0; lvl--) {
        if (!busy->queue_head[lvl]) continue;

        struct process *prev = NULL;
        struct process *cur = busy->queue_head[lvl];

        /* Find the last (tail) process in this priority level */
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

    context_switch(current ? &current->context : NULL, next->context);
    __asm__ volatile("sti");
}

void scheduler_yield(void) {
    schedule();
}

uint64_t scheduler_get_idle_ticks(void) {
    return this_cpu()->idle_ticks;
}

/* ── Timer tick handler ─────────────────────────────────────────────── */
void scheduler_tick(int was_user) {
    struct cpu_info *ci = this_cpu();
    if (!ci->scheduler_enabled) return;

    struct process *cur = ci->current_process;
    if (!cur || cur->state != PROCESS_RUNNING) return;

    /* Account CPU time: user time if we interrupted user code,
     * system time if we interrupted kernel code. */
    if (was_user && cur->is_user) {
        cur->utime_ticks++;
    } else {
        cur->stime_ticks++;
    }

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
            cur->ticks_remaining = time_slices[lvl] * 2;
        } else {
            /* SCHED_OTHER / SCHED_RR: preempt on quantum expiry.
             * SCHED_RR places the process at the end of its priority queue
             * (handled by schedule() re-adding it). */
            schedule();
        }
    }
}

/* ── Aging: boost starved processes ─────────────────────────────────── */
void scheduler_age(void) {
    uint64_t now = timer_get_ticks();
    struct process *table = process_get_table();

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&sched_lock, &irq_flags);

    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &table[i];
        if (p->state != PROCESS_READY) continue;
        if (p->priority == 0) continue;
        if (p->last_run_tick == 0) continue;
        if (now - p->last_run_tick > 200) {
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
