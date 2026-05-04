#include "scheduler.h"
#include "process.h"
#include "signal.h"
#include "io.h"
#include "gdt.h"
#include "vmm.h"
#include "timer.h"

extern void process_set_current(struct process *proc);
extern uint64_t syscall_kernel_rsp;

static struct process *ready_queue_head = NULL;
static struct process *ready_queue_tail = NULL;
static int scheduler_enabled = 0;

void scheduler_init(void) {
    ready_queue_head = NULL;
    ready_queue_tail = NULL;
    scheduler_enabled = 1;
}

void scheduler_add(struct process *proc) {
    proc->next = NULL;
    if (!ready_queue_tail) {
        ready_queue_head = proc;
        ready_queue_tail = proc;
    } else {
        ready_queue_tail->next = proc;
        ready_queue_tail = proc;
    }
}

void scheduler_remove(struct process *proc) {
    struct process *prev = NULL;
    struct process *cur = ready_queue_head;

    while (cur) {
        if (cur == proc) {
            if (prev) prev->next = cur->next;
            else ready_queue_head = cur->next;
            if (cur == ready_queue_tail) ready_queue_tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

void schedule(void) {
    if (!scheduler_enabled) return;

    struct process *current = process_get_current();
    struct process *next = ready_queue_head;

    if (!next) return; /* no ready processes, keep running current */

    /* Remove next from head of ready queue */
    ready_queue_head = next->next;
    if (!ready_queue_head) ready_queue_tail = NULL;
    next->next = NULL;

    /* Put current back in ready queue if it's still runnable */
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

    /* Check and deliver any pending signals before returning to next process */
    signal_check();

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
