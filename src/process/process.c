#include "process.h"
#include "heap.h"
#include "string.h"
#include "scheduler.h"

extern void process_entry_trampoline(void);
extern void user_entry_trampoline(void);

static struct process process_table[PROCESS_MAX];
static uint32_t next_pid = 1;
static struct process *current_process = NULL;

void process_init(void) {
    memset(process_table, 0, sizeof(process_table));

    /* Create idle process (pid 0) - represents the boot thread */
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_RUNNING;
    process_table[0].name = "idle";
    process_table[0].pending_signals = 0;
    process_table[0].is_user = 0;
    process_table[0].pml4 = NULL;
    memset(process_table[0].sig_handlers, 0, sizeof(process_table[0].sig_handlers));
    current_process = &process_table[0];
}

struct process *process_create(void (*entry)(void), const char *name) {
    struct process *proc = NULL;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            proc = &process_table[i];
            break;
        }
    }
    if (!proc) return NULL;

    /* Allocate kernel stack */
    uint8_t *stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!stack) return NULL;

    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    proc->name = name;
    proc->kernel_stack = (uint64_t)stack;
    proc->stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE);
    proc->next = NULL;
    proc->pending_signals = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    proc->is_user = 0;
    proc->user_entry = 0;
    proc->user_rsp = 0;
    proc->pml4 = NULL;

    /* Set up initial context on the stack */
    uint64_t *sp = (uint64_t *)(proc->stack_top);

    /* context_switch will pop: r15, r14, r13, r12, rbx, rbp, then ret to rip.
     * For new processes, ret goes to process_entry_trampoline which does sti
     * then jmp r15 (the real entry point). This is needed because schedule()
     * does cli before context_switch. */
    sp -= 7;
    sp[0] = (uint64_t)entry;   /* r15 = real entry point */
    sp[1] = 0;                  /* r14 */
    sp[2] = 0;                  /* r13 */
    sp[3] = 0;                  /* r12 */
    sp[4] = 0;                  /* rbx */
    sp[5] = 0;                  /* rbp */
    sp[6] = (uint64_t)process_entry_trampoline;  /* rip = trampoline */

    proc->context = (struct cpu_context *)sp;

    scheduler_add(proc);
    return proc;
}

struct process *process_create_user(uint64_t entry, uint64_t user_rsp,
                                    uint64_t *pml4, const char *name) {
    struct process *proc = NULL;

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            proc = &process_table[i];
            break;
        }
    }
    if (!proc) return NULL;

    /* Allocate kernel stack for syscall handling */
    uint8_t *stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!stack) return NULL;

    proc->pid = next_pid++;
    proc->state = PROCESS_READY;
    proc->name = name;
    proc->kernel_stack = (uint64_t)stack;
    proc->stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE);
    proc->next = NULL;
    proc->pending_signals = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    proc->is_user = 1;
    proc->user_entry = entry;
    proc->user_rsp = user_rsp;
    proc->pml4 = pml4;

    /* Set up initial context on kernel stack.
     * context_switch will pop r15..rbp then ret → user_entry_trampoline
     * which does iretq to ring 3.
     * r15 = user RIP, r14 = user RSP */
    uint64_t *sp = (uint64_t *)(proc->stack_top);
    sp -= 7;
    sp[0] = entry;          /* r15 = user entry point */
    sp[1] = user_rsp;       /* r14 = user stack pointer */
    sp[2] = 0;              /* r13 */
    sp[3] = 0;              /* r12 */
    sp[4] = 0;              /* rbx */
    sp[5] = 0;              /* rbp */
    sp[6] = (uint64_t)user_entry_trampoline;  /* rip = ring3 trampoline */

    proc->context = (struct cpu_context *)sp;

    scheduler_add(proc);
    return proc;
}

void process_exit(void) {
    current_process->state = PROCESS_ZOMBIE;
    scheduler_remove(current_process);
    scheduler_yield();
    /* should never reach here */
    for (;;) __asm__ volatile("hlt");
}

struct process *process_get_current(void) {
    return current_process;
}

void process_set_current(struct process *proc) {
    current_process = proc;
}

struct process *process_get_by_pid(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED && process_table[i].pid == pid)
            return &process_table[i];
    }
    return NULL;
}

/* For shell `ps` command */
struct process *process_get_table(void) {
    return process_table;
}
