#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "vmm.h"
#include "pmm.h"
#include "smp.h"
#include "signal.h"
#include "syscall.h"

static struct process process_table[PROCESS_MAX];
extern void user_entry_trampoline(void);
extern void process_entry_trampoline(void);
static uint32_t next_pid = 1;
static struct process *current_process = NULL;

/* Allocate a unique PID, avoiding wrap-around aliasing */
static uint32_t alloc_pid(void) {
    uint32_t pid = next_pid++;
    if (next_pid == 0 || next_pid > 0xFFFF) {
        /* Near wrap — scan for an unused PID and reset */
        next_pid = 1;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (process_table[i].state != PROCESS_UNUSED &&
                process_table[i].pid >= next_pid &&
                process_table[i].pid <= 0xFFFF) {
                next_pid = process_table[i].pid + 1;
            }
        }
    }
    return pid;
}

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
    process_caps_allow(proc, SYS_FD_READ);
    process_caps_allow(proc, SYS_FD_WRITE);

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
    /* TCP server syscalls */
    process_caps_allow(proc, SYS_NET_TCP_LISTEN);
    process_caps_allow(proc, SYS_NET_TCP_ACCEPT);
    process_caps_allow(proc, SYS_NET_TCP_SEND_CONN);
    process_caps_allow(proc, SYS_NET_TCP_RECV_CONN);
    process_caps_allow(proc, SYS_NET_TCP_CLOSE_CONN);
    process_caps_allow(proc, SYS_NET_TCP_UNLISTEN);

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
    process_table[0].sig_mask = 0;
    process_table[0].is_user = 0;
    process_table[0].pml4 = NULL;
    process_table[0].parent_pid = 0;
    process_table[0].pgid = 0;
    process_table[0].sid = 0;
    process_table[0].exit_code = 0;
    process_table[0].sleep_until = 0;
    process_table[0].is_background = 0;
    process_table[0].is_suspended = 0;
    process_table[0].priority = 1;
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

    proc->pid = alloc_pid();
    proc->state = PROCESS_READY;
    proc->name = name;
    proc->kernel_stack = (uint64_t)stack;
    proc->stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE);
    proc->next = NULL;
    proc->pending_signals = 0;
    proc->sig_mask = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
    proc->is_user = 0;
    proc->user_entry = 0;
    proc->user_rsp = 0;
    proc->pml4 = NULL;
    proc->parent_pid  = current_process ? current_process->pid : 0;
    proc->pgid = current_process ? current_process->pgid : proc->pid;
    proc->sid = current_process ? current_process->sid : proc->pid;
    proc->exit_code   = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->priority    = 1; /* normal priority */
    proc->wait_for_pid   = 0;
    proc->ticks_remaining = 0; /* set by scheduler on first run */
    proc->last_run_tick  = timer_get_ticks();
    /* Inherit cwd from parent */
    if (current_process && current_process->cwd[0])
        strncpy(proc->cwd, current_process->cwd, 63);
    else
        strncpy(proc->cwd, "/", 63);
    proc->cwd[63] = '\0';
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

    proc->pid = alloc_pid();
    proc->state = PROCESS_READY;
    proc->name = name;
    proc->kernel_stack = (uint64_t)stack;
    proc->stack_top = (uint64_t)(stack + KERNEL_STACK_SIZE);
    proc->next = NULL;
    proc->pending_signals = 0;
    proc->sig_mask = 0;
    memset(proc->sig_handlers, 0, sizeof(proc->sig_handlers));
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
    proc->is_user = 1;
    proc->user_entry = entry;
    proc->user_rsp = user_rsp;
    proc->pml4 = pml4;
    proc->parent_pid = current_process ? current_process->pid : 0;
    proc->pgid = current_process ? current_process->pgid : proc->pid;
    proc->sid = current_process ? current_process->sid : proc->pid;
    proc->exit_code = 0;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->priority = 1;
    proc->wait_for_pid   = 0;
    proc->ticks_remaining = 0;
    proc->last_run_tick  = timer_get_ticks();
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

/* Wake any process blocked in waitpid waiting for 'pid'. */
static void process_wake_waiter(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        struct process *p = &process_table[i];
        if (p->state == PROCESS_BLOCKED && p->wait_for_pid == pid) {
            p->wait_for_pid = 0;
            p->state = PROCESS_READY;
            p->last_run_tick = timer_get_ticks();
            scheduler_add(p);
        }
    }
}

void process_exit(void) {
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = 0;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    scheduler_yield();
    /* should never reach here */
    for (;;) __asm__ volatile("hlt");
}

void process_exit_code(int code) {
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = code;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    scheduler_yield();
    for (;;) __asm__ volatile("hlt");
}

struct process *process_get_current(void) {
    struct process *proc = get_current_process();
    if (!proc) return current_process;
    return proc;
}

/*
 * fork() — clone current process, child starts at fork_child_entry.
 * Returns child PID to parent, -1 on error.
 * NOTE: The child does NOT return from process_fork() with value 0.
 * Instead, the child begins execution in fork_child_entry().
 */
int process_fork(void);
extern void fork_child_trampoline(void); /* defined in switch.asm */

static void fork_child_entry(void) {
    process_exit_code(0);
}

int process_fork(void) {
    struct process *parent = current_process;
    struct process *child = NULL;

    __asm__ volatile("cli");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            child = &process_table[i];
            break;
        }
    }
    if (!child) { __asm__ volatile("sti"); return -1; }

    child->state = PROCESS_UNUSED;
    *child = *parent;
    child->pid = alloc_pid();
    child->parent_pid = parent->pid;
    child->is_suspended = 0;

    /* Allocate fresh kernel stack BEFORE setting state to READY */
    uint8_t *new_stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!new_stack) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -1;
    }
    child->kernel_stack = (uint64_t)new_stack;
    child->stack_top    = (uint64_t)(new_stack + KERNEL_STACK_SIZE);
    child->state = PROCESS_READY;

    /* Clone user address space if process has one */
    if (parent->pml4) {
        child->pml4 = vmm_clone_user_pml4(parent->pml4);
        if (!child->pml4) {
            kfree(new_stack);
            child->state = PROCESS_UNUSED;
            __asm__ volatile("sti");
            return -1;
        }
        /* Flush parent TLB: COW marking made parent pages read-only in the page tables */
        vmm_switch_pml4(parent->pml4);
    }

    /* Set up child kernel stack */
    uint64_t *sp = (uint64_t *)child->stack_top;
    sp -= 7;
    sp[0] = (uint64_t)fork_child_entry;  /* r15 = child entry point */
    sp[1] = 0;  sp[2] = 0; sp[3] = 0; sp[4] = 0; sp[5] = 0;
    sp[6] = (uint64_t)fork_child_trampoline;
    child->context = (struct cpu_context *)sp;

    scheduler_add(child);
    __asm__ volatile("sti");
    return (int)child->pid;
}

/* ── Clone: create a thread (child may share address space) ──── */
extern void clone_child_trampoline(void);

int process_clone(struct process *parent, uint64_t flags, void *child_stack,
                  uint64_t user_rip, uint64_t user_rflags) {
    struct process *child = NULL;

    __asm__ volatile("cli");

    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state == PROCESS_UNUSED) {
            child = &process_table[i];
            break;
        }
    }
    if (!child) { __asm__ volatile("sti"); return -1; }

    child->state = PROCESS_UNUSED;
    *child = *parent;
    child->pid = alloc_pid();
    child->parent_pid = parent->pid;
    child->is_suspended = 0;
    child->wait_for_pid = 0;
    child->on_queue = 0;
    child->context = NULL;
    child->next = NULL;
    child->tgid = (flags & CLONE_THREAD) ? parent->tgid : child->pid;

    /* Allocate fresh kernel stack */
    uint8_t *new_stack = (uint8_t *)kmalloc(KERNEL_STACK_SIZE);
    if (!new_stack) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -1;
    }
    child->kernel_stack = (uint64_t)new_stack;
    child->stack_top    = (uint64_t)(new_stack + KERNEL_STACK_SIZE);

    /* Handle CLONE_VM — share address space */
    if (flags & CLONE_VM) {
        /* Child shares parent's page tables — no copy needed */
        /* Kernel page tables are already shared via the higher-half mapping */
        if (parent->pml4) {
            child->pml4 = parent->pml4;
        }
    } else {
        /* Full fork-style: deep-copy user address space */
        if (parent->pml4) {
            child->pml4 = vmm_clone_user_pml4(parent->pml4);
            if (!child->pml4) {
                kfree(new_stack);
                child->state = PROCESS_UNUSED;
                __asm__ volatile("sti");
                return -1;
            }
            vmm_switch_pml4(parent->pml4);
        }
    }

    /* Handle CLONE_FILES — share FD table */
    if (flags & CLONE_FILES) {
        /* Child shares parent's FD table — no-op since struct copy above inherited it */
    } else {
        /* Private FD table: already copied from parent, no extra work needed */
    }

    child->state = PROCESS_READY;

    /* Set up child's kernel stack with sysret return frame.
     * Layout (from stack_top down):
     *   [context_switch frame: r15..rbp, rip → clone_child_trampoline]
     *   [syscall return frame: r15..rbp, r11, rcx, user_rsp]
     */
    uint64_t *sp = (uint64_t *)child->stack_top;

    /* Syscall return frame (9 values, bottom): */
    sp -= 9;
    sp[0] = 0;                    /* junk r15 (unused) */
    sp[1] = 0;                    /* junk r14 */
    sp[2] = 0;                    /* junk r13 */
    sp[3] = 0;                    /* junk r12 */
    sp[4] = 0;                    /* junk rbx */
    sp[5] = 0;                    /* junk rbp */
    sp[6] = user_rflags;         /* r11 → user RFLAGS for sysret */
    sp[7] = user_rip;            /* rcx → user RIP for sysret */
    sp[8] = (uint64_t)child_stack; /* user RSP for sysret */

    /* Context switch frame (7 values, above): */
    sp -= 7;
    sp[0] = 0;                    /* r15 */
    sp[1] = 0;                    /* r14 */
    sp[2] = 0;                    /* r13 */
    sp[3] = 0;                    /* r12 */
    sp[4] = 0;                    /* rbx */
    sp[5] = 0;                    /* rbp */
    sp[6] = (uint64_t)clone_child_trampoline;  /* rip → trampoline */

    child->context = (struct cpu_context *)sp;

    scheduler_add(child);
    __asm__ volatile("sti");
    return (int)child->pid;
}

void process_set_current(struct process *proc) {
    set_current_process(proc);
    current_process = proc;
}

struct process *process_get_by_pid(uint32_t pid) {
    struct process *fallback = NULL;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED && process_table[i].pid == pid) {
            /* Prefer non-zombie processes (live) over zombie (dead) */
            if (process_table[i].state != PROCESS_ZOMBIE)
                return &process_table[i];
            if (!fallback) fallback = &process_table[i];
        }
    }
    return fallback;
}

/* For shell `ps` command */
struct process *process_get_table(void) {
    return process_table;
}

/* Wait for a specific child process to become ZOMBIE.
 * Returns 0 on success (exit code in *status), -1 if not found.
 * Blocks (does NOT spin) until the child exits. */
int process_waitpid(uint32_t pid, int *status) {
    struct process *child = process_get_by_pid(pid);
    if (!child) return -1;

    if (child->state != PROCESS_ZOMBIE && child->state != PROCESS_UNUSED) {
        /* Block until child's process_exit_code wakes us */
        current_process->wait_for_pid = pid;
        current_process->state = PROCESS_BLOCKED;
        scheduler_remove(current_process);
        scheduler_yield();
        /* After wake, re-lookup the child — it may have been cleaned up
         * by another concurrent waitpid (race). */
        current_process->wait_for_pid = 0;
        child = process_get_by_pid(pid);
        if (!child || child->state == PROCESS_UNUSED) return -1;
    }

    if (status) *status = child->exit_code;
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
    /* Free user page tables (fixes leak) */
    if (proc->is_user && proc->pml4) {
        vmm_destroy_user_pml4(proc->pml4);
        proc->pml4 = NULL;
    }
    proc->state = PROCESS_UNUSED;
    proc->pid = 0;
    proc->name = NULL;
    proc->context = NULL;
    proc->next = NULL;
    proc->sleep_until = 0;
    proc->is_background = 0;
    proc->is_suspended = 0;
    proc->pgid = 0;
    proc->sid = 0;
    proc->priority = 1;
    proc->cap_profile = PROCESS_CAP_PROFILE_NONE;
    process_caps_clear_all(proc);
    memset(proc->fd_table, 0, sizeof(proc->fd_table));
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
