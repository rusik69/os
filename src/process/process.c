#include "process.h"
#include "heap.h"
#include "string.h"
#include "scheduler.h"
#include "timer.h"
#include "syscall.h"

extern void process_entry_trampoline(void);
extern void user_entry_trampoline(void);

static struct process process_table[PROCESS_MAX];
static uint32_t next_pid = 1;
static struct process *current_process = NULL;

static inline int process_cap_valid(uint32_t num) {
    return num < PROCESS_SYSCALL_MAX;
}

void process_caps_clear_all(struct process *proc) {
    if (!proc) return;
    memset(proc->syscall_caps, 0, sizeof(proc->syscall_caps));
}

void process_caps_allow(struct process *proc, uint32_t num) {
    if (!proc || !process_cap_valid(num)) return;
    proc->syscall_caps[num / 64] |= (1ULL << (num % 64));
}

void process_caps_allow_all(struct process *proc) {
    if (!proc) return;
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
        proc->syscall_caps[i] = ~0ULL;
    }
}

int process_caps_has(const struct process *proc, uint32_t num) {
    if (!proc || !process_cap_valid(num)) return 0;
    return (proc->syscall_caps[num / 64] & (1ULL << (num % 64))) != 0;
}

static void process_caps_apply_user_default(struct process *proc) {
    process_caps_clear_all(proc);

    /* Core process/syscall lifecycle */
    process_caps_allow(proc, SYS_READ);
    process_caps_allow(proc, SYS_WRITE);
    process_caps_allow(proc, SYS_OPEN);
    process_caps_allow(proc, SYS_CLOSE);
    process_caps_allow(proc, SYS_EXIT);
    process_caps_allow(proc, SYS_GETPID);
    process_caps_allow(proc, SYS_KILL);
    process_caps_allow(proc, SYS_BRK);
    process_caps_allow(proc, SYS_STAT);
    process_caps_allow(proc, SYS_MKDIR);
    process_caps_allow(proc, SYS_UNLINK);
    process_caps_allow(proc, SYS_TIME);
    process_caps_allow(proc, SYS_YIELD);
    process_caps_allow(proc, SYS_UPTIME);
    process_caps_allow(proc, SYS_WAITPID);
    process_caps_allow(proc, SYS_SLEEP_TICKS);

    /* Filesystem/VFS through syscall boundary */
    process_caps_allow(proc, SYS_FS_CREATE);
    process_caps_allow(proc, SYS_FS_WRITE);
    process_caps_allow(proc, SYS_FS_READ);
    process_caps_allow(proc, SYS_FS_DELETE);
    process_caps_allow(proc, SYS_FS_LIST);
    process_caps_allow(proc, SYS_FS_STAT);
    process_caps_allow(proc, SYS_FS_STAT_EX);
    process_caps_allow(proc, SYS_FS_CHMOD);
    process_caps_allow(proc, SYS_FS_CHOWN);
    process_caps_allow(proc, SYS_FS_GET_USAGE);
    process_caps_allow(proc, SYS_FS_LIST_NAMES);
    process_caps_allow(proc, SYS_VFS_READ);
    process_caps_allow(proc, SYS_VFS_WRITE);
    process_caps_allow(proc, SYS_VFS_STAT);
    process_caps_allow(proc, SYS_VFS_CREATE);
    process_caps_allow(proc, SYS_VFS_UNLINK);
    process_caps_allow(proc, SYS_VFS_READDIR);

    /* Network stack access */
    process_caps_allow(proc, SYS_NET_PRESENT);
    process_caps_allow(proc, SYS_NET_GET_MAC);
    process_caps_allow(proc, SYS_NET_GET_IP);
    process_caps_allow(proc, SYS_NET_GET_GW);
    process_caps_allow(proc, SYS_NET_GET_MASK);
    process_caps_allow(proc, SYS_NET_DNS);
    process_caps_allow(proc, SYS_NET_PING);
    process_caps_allow(proc, SYS_NET_UDP_SEND);
    process_caps_allow(proc, SYS_NET_HTTP_GET);
    process_caps_allow(proc, SYS_NET_ARP_LIST);

    /* User program execution helpers */
    process_caps_allow(proc, SYS_ELF_EXEC);
    process_caps_allow(proc, SYS_SCRIPT_EXEC);
}

static void process_caps_apply_user_trusted(struct process *proc) {
    process_caps_allow_all(proc);
}

int process_set_cap_profile(struct process *proc, enum process_cap_profile profile) {
    if (!proc) return -1;

    switch (profile) {
        case PROCESS_CAP_PROFILE_NONE:
            process_caps_clear_all(proc);
            break;
        case PROCESS_CAP_PROFILE_USER_DEFAULT:
            process_caps_apply_user_default(proc);
            break;
        case PROCESS_CAP_PROFILE_USER_TRUSTED:
            process_caps_apply_user_trusted(proc);
            break;
        default:
            return -1;
    }

    proc->cap_profile = (uint8_t)profile;
    return 0;
}

void process_init(void) {
    memset(process_table, 0, sizeof(process_table));

    /* Create idle process (pid 0) - represents the boot thread */
    process_table[0].pid = 0;
    process_table[0].state = PROCESS_RUNNING;
    process_table[0].name = "idle";
    process_table[0].pending_signals = 0;
    process_table[0].is_user = 0;
    process_table[0].pml4 = NULL;
    process_table[0].parent_pid = 0;
    process_table[0].exit_code = 0;
    process_table[0].sleep_until = 0;
    process_table[0].is_background = 0;
    process_table[0].cap_profile = PROCESS_CAP_PROFILE_USER_TRUSTED;
    process_caps_allow_all(&process_table[0]);
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
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->exit_code = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_TRUSTED);

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
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->exit_code = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_DEFAULT);

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
    current_process->exit_code = 0;
    scheduler_remove(current_process);
    scheduler_yield();
    /* should never reach here */
    for (;;) __asm__ volatile("hlt");
}

void process_exit_code(int code) {
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = code;
    scheduler_remove(current_process);
    scheduler_yield();
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

/* Wait for a specific child process to become ZOMBIE.
 * Returns 0 on success (exit code in *status), -1 if not found. */
int process_waitpid(uint32_t pid, int *status) {
    struct process *child = process_get_by_pid(pid);
    if (!child) return -1;
    /* Spin-yield until child becomes zombie */
    while (child->state != PROCESS_ZOMBIE && child->state != PROCESS_UNUSED) {
        scheduler_yield();
    }
    if (status) *status = child->exit_code;
    /* Reap the zombie */
    process_cleanup(child);
    return 0;
}

/* Block the current process for N ticks. */
void process_sleep_ticks(uint64_t nticks) {
    current_process->sleep_until = timer_get_ticks() + nticks;
    current_process->state = PROCESS_BLOCKED;
    scheduler_remove(current_process);
    scheduler_yield();
}

/* Free resources of a zombie process. */
void process_cleanup(struct process *proc) {
    if (proc->kernel_stack) {
        kfree((void *)proc->kernel_stack);
        proc->kernel_stack = 0;
    }
    proc->state = PROCESS_UNUSED;
    proc->pid = 0;
    proc->name = NULL;
    proc->context = NULL;
    proc->next = NULL;
    proc->sleep_until = 0;
    proc->cap_profile = PROCESS_CAP_PROFILE_NONE;
    process_caps_clear_all(proc);
}

/* Reap zombie processes: background jobs are reaped immediately,
 * other zombies are reaped only if their parent is gone. */
void process_reap_zombies(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_ZOMBIE) {
            /* Background processes: reap immediately (no one waits for them) */
            if (process_table[i].is_background) {
                process_cleanup(&process_table[i]);
                continue;
            }
            /* Non-background: reap if parent is gone */
            struct process *parent = process_get_by_pid(process_table[i].parent_pid);
            if (!parent || parent->state == PROCESS_ZOMBIE ||
                parent->state == PROCESS_UNUSED) {
                process_cleanup(&process_table[i]);
            }
        }
    }
}
