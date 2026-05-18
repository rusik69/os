#include "scheduler.h"
#include "process.h"
#include "io.h"
#include "gdt.h"
#include "vmm.h"
#include "timer.h"

extern void process_set_current(struct process *proc);
extern uint64_t syscall_kernel_rsp;

/* 4-level multilevel priority queue: 0 = highest, 3 = lowest */
#define SCHED_LEVELS 4
static struct process *queue_head[SCHED_LEVELS];
static struct process *queue_tail[SCHED_LEVELS];
static int scheduler_enabled = 0;

void scheduler_init(void) {
    for (int i = 0; i < SCHED_LEVELS; i++) {
        queue_head[i] = NULL;
        queue_tail[i] = NULL;
    }
    scheduler_enabled = 1;
}

/* Add a process to its priority queue */
void scheduler_add(struct process *proc) {
    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
    proc->next = NULL;
    if (!queue_tail[lvl]) {
        queue_head[lvl] = proc;
        queue_tail[lvl] = proc;
    } else {
        queue_tail[lvl]->next = proc;
        queue_tail[lvl] = proc;
    }
}

/* Remove a process from whatever queue it is in */
void scheduler_remove(struct process *proc) {
    int lvl = (int)proc->priority;
    if (lvl < 0 || lvl >= SCHED_LEVELS) lvl = 1;
    struct process *prev = NULL;
    struct process *cur  = queue_head[lvl];
    while (cur) {
        if (cur == proc) {
            if (prev) prev->next = cur->next;
            else queue_head[lvl] = cur->next;
            if (cur == queue_tail[lvl]) queue_tail[lvl] = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur  = cur->next;
    }
}

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

/* Pick the highest-priority non-empty queue */
static struct process *dequeue_next(void) {
    for (int lvl = 0; lvl < SCHED_LEVELS; lvl++) {
        if (queue_head[lvl]) {
            struct process *p = queue_head[lvl];
            queue_head[lvl] = p->next;
            if (!queue_head[lvl]) queue_tail[lvl] = NULL;
            p->next = NULL;
            return p;
        }
    }
    return NULL;
}

void schedule(void) {
    if (!scheduler_enabled) return;

    struct process *current = process_get_current();
    struct process *next = dequeue_next();

    if (!next) return; /* no ready processes, keep running current */

    /* Put current back in its priority queue if still runnable */
    if (current->state == PROCESS_RUNNING) {
        current->state = PROCESS_READY;
        scheduler_add(current);
    }

    next->state = PROCESS_RUNNING;
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

    __asm__ volatile("cli");
    context_switch(&current->context, next->context);
    __asm__ volatile("sti");
}

void scheduler_yield(void) {
    schedule();
}

/* Wake any processes whose sleep timer has expired. Called from timer interrupt. */
void scheduler_wake_sleepers(void) {
    uint64_t now = timer_get_ticks();
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_BLOCKED && table[i].sleep_until > 0 &&
            now >= table[i].sleep_until) {
            table[i].sleep_until = 0;
            table[i].state = PROCESS_READY;
            scheduler_add(&table[i]);
        }
    }
}

