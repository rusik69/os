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
static struct process *current_process = NULL;

/* PID bitmap allocator: 256 processes → 4 × uint64_t.
 * Bit N ≡ PID N allocated.  Bit 0 always set (idle process).
 * O(1) alloc via __builtin_ctzll on inverted word. */
static uint64_t pid_bitmap[4];
#define PID_BITMAP_WORDS 4

static uint32_t alloc_pid(void) {
    for (int w = 0; w < PID_BITMAP_WORDS; w++) {
        if (pid_bitmap[w] == ~0ULL) continue;
        int bit = __builtin_ctzll(~pid_bitmap[w]);
        uint32_t pid = (uint32_t)(w * 64 + bit);
        if (pid >= PROCESS_MAX) break;
        pid_bitmap[w] |= (1ULL << bit);
        return pid;
    }
    return (uint32_t)-1; /* no free PIDs */
}

static void free_pid(uint32_t pid) {
    if (pid >= PROCESS_MAX) return;
    int w = pid / 64;
    int bit = pid % 64;
    pid_bitmap[w] &= ~(1ULL << bit);
}

/* ── Kernel stack with guard page ───────────────────────────────────── */

/* Allocate a kernel stack with an unmapped guard page at the bottom.
 * Stack grows downward: [guard (unmapped)][kernel_stack ... stack_top]
 * A stack overflow past kernel_stack will hit the guard page and fault.
 * Returns 0 on success, -1 on failure (all frames freed on error). */
static int alloc_guarded_kernel_stack(struct process *proc) {
    uint64_t *phys = pmm_alloc_frames(KERNEL_STACK_TOTAL_PAGES);
    if (!phys) return -1;

    uint64_t guard_phys  = (uint64_t)phys;
    uint64_t stack_phys  = guard_phys + PAGE_SIZE;
    uint64_t guard_vma   = (uint64_t)PHYS_TO_VIRT(guard_phys);
    uint64_t stack_vma   = (uint64_t)PHYS_TO_VIRT(stack_phys);

    /* Call vmm_map_page for the guard VMA — this splits the 2MB huge page
     * into 4KB PTEs for the region if needed.  Then unmap just the guard. */
    if (vmm_map_page(guard_vma, guard_phys, VMM_FLAG_WRITE) < 0) {
        for (size_t i = 0; i < KERNEL_STACK_TOTAL_PAGES; i++)
            pmm_free_frame(guard_phys + i * PAGE_SIZE);
        return -1;
    }
    vmm_unmap_page(guard_vma);

    proc->guard_page    = guard_vma;
    proc->kernel_stack  = stack_vma;
    proc->stack_top     = stack_vma + KERNEL_STACK_SIZE;
    return 0;
}

/* Free a kernel stack previously allocated by alloc_guarded_kernel_stack.
 * Re-maps the guard page first so freeing its physical frame is safe. */
static void free_guarded_kernel_stack(struct process *proc) {
    if (!proc->kernel_stack) return;

    uint64_t guard_phys = VIRT_TO_PHYS(proc->guard_page);

    /* Re-map guard page so the PTEs are consistent while we free */
    vmm_map_page(proc->guard_page, guard_phys, VMM_FLAG_WRITE);

    for (size_t i = 0; i < KERNEL_STACK_TOTAL_PAGES; i++)
        pmm_free_frame(guard_phys + i * PAGE_SIZE);

    proc->guard_page    = 0;
    proc->kernel_stack  = 0;
    proc->stack_top     = 0;
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
    /* Check capability and bounding set */
    if (!(proc->syscall_caps[num / 64] & (1ULL << (num % 64)))) return 0;
    /* Also check bounding set */
    if (!(proc->cap_bset[num / 64] & (1ULL << (num % 64)))) return 0;
    return 1;
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

static void rlimit_init_defaults(struct process *proc) {
    /* Default resource limits (RLIM_INFINITY for most) */
    for (int i = 0; i < _RLIMIT_NLIMITS; i++) {
        proc->rlim_cur[i] = ~0ULL;
        proc->rlim_max[i] = ~0ULL;
    }
    /* Set sensible defaults */
    proc->rlim_cur[5] = 256;   /* RLIMIT_NOFILE = 256 */
    proc->rlim_max[5] = 256;
    proc->rlim_cur[7] = 64;    /* RLIMIT_NPROC = 64 */
    proc->rlim_max[7] = 64;
    proc->rlim_cur[0] = 1024ULL * 1024 * 1024;  /* RLIMIT_AS = 1GB */
    proc->rlim_max[0] = 1024ULL * 1024 * 1024;
    proc->rlim_cur[1] = 1024ULL * 1024;          /* RLIMIT_CORE = 1MB */
    proc->rlim_max[1] = 1024ULL * 1024;
    proc->rlim_cur[6] = 1024ULL * 64;            /* RLIMIT_STACK = 64KB */
    proc->rlim_max[6] = 1024ULL * 64;
}

void process_init(void) {
    memset(process_table, 0, sizeof(process_table));
    memset(pid_bitmap, 0, sizeof(pid_bitmap));
    pid_bitmap[0] = 1; /* PID 0 (idle) is always allocated */

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
    process_table[0].base_priority = 1;
    process_table[0].cpu_affinity = 0;  /* allow any CPU */
    process_table[0].uid = 0;     /* root */
    process_table[0].gid = 0;
    process_table[0].euid = 0;
    process_table[0].egid = 0;
    process_table[0].umask = 0022;  /* default: rwxr-xr-x */
    memset(process_table[0].itimers, 0, sizeof(process_table[0].itimers));
    process_table[0].cap_profile = PROCESS_CAP_PROFILE_USER_TRUSTED;
    process_caps_allow_all(&process_table[0]);
    memset(process_table[0].sig_handlers, 0, sizeof(process_table[0].sig_handlers));
    process_table[0].sched_policy = SCHED_OTHER;
    process_table[0].alt_stack_sp = NULL;
    process_table[0].alt_stack_size = 0;
    process_table[0].alt_stack_flags = SS_DISABLE;
    process_table[0].personality = 0;
    process_table[0].coredump_enabled = 1;
    memset(process_table[0].proc_comm, 0, 16);
    rlimit_init_defaults(&process_table[0]);
    cap_bset_init(&process_table[0]);
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

    /* Enforce RLIMIT_NPROC: check total process count */
    struct process *cur = process_get_current();
    if (cur) {
        int total_count = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (process_table[i].state != PROCESS_UNUSED)
                total_count++;
        }
        if ((uint64_t)total_count >= cur->rlim_cur[RLIMIT_NPROC])
            return NULL;
    }

    /* Allocate kernel stack with guard page */
    if (alloc_guarded_kernel_stack(proc) < 0) return NULL;

    proc->pid = alloc_pid();
    proc->state = PROCESS_READY;
    proc->name = name;
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
    proc->cpu_affinity = 0; /* allow any CPU */
    proc->uid = 0;
    proc->gid = 0;
    proc->euid = 0;
    proc->egid = 0;
    proc->umask = 0022;
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
    proc->sched_policy = SCHED_OTHER;
    proc->alt_stack_sp = NULL;
    proc->alt_stack_size = 0;
    proc->alt_stack_flags = SS_DISABLE;
    proc->personality = 0;
    proc->coredump_enabled = 1;
    memset(proc->proc_comm, 0, 16);
    memset(proc->itimers, 0, sizeof(proc->itimers));
    rlimit_init_defaults(proc);
    cap_bset_init(proc);

    /* Initialize CPU time accounting */
    proc->utime_ticks = 0;
    proc->stime_ticks = 0;
    proc->start_time_tick = timer_get_ticks();
    proc->nvcsw = 0;
    proc->nivcsw = 0;
    proc->minflt = 0;
    proc->majflt = 0;

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
    if (alloc_guarded_kernel_stack(proc) < 0) return NULL;

    proc->pid = alloc_pid();
    proc->state = PROCESS_READY;
    proc->name = name;
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
    proc->cpu_affinity = 0;
    proc->base_priority = 1;
    proc->uid = 0; proc->gid = 0; proc->euid = 0; proc->egid = 0;
    proc->umask = 0022;
    proc->wait_for_pid   = 0;
    proc->ticks_remaining = 0;
    proc->last_run_tick  = timer_get_ticks();
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_DEFAULT);
    proc->sched_policy = SCHED_OTHER;
    proc->alt_stack_sp = NULL;
    proc->alt_stack_size = 0;
    proc->alt_stack_flags = SS_DISABLE;
    proc->personality = 0;
    proc->coredump_enabled = 1;
    memset(proc->proc_comm, 0, 16);
    rlimit_init_defaults(proc);
    cap_bset_init(proc);

    /* Inherit parent's bounding set */
    if (current_process && current_process->state != PROCESS_UNUSED) {
        for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++)
            proc->cap_bset[i] = current_process->cap_bset[i];
    }

    /* Initialize CPU time accounting */
    proc->utime_ticks = 0;
    proc->stime_ticks = 0;
    proc->start_time_tick = timer_get_ticks();
    proc->nvcsw = 0;
    proc->nivcsw = 0;
    proc->minflt = 0;
    proc->majflt = 0;

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
    /* Send SIGCHLD to parent */
    struct process *parent = process_get_by_pid(current_process->parent_pid);
    if (parent) {
        struct siginfo info;
        info.si_signo = SIGCHLD;
        info.si_errno = 0;
        info.si_code  = CLD_EXITED;
        info.si_pid   = current_process->pid;
        info.si_uid   = current_process->uid;
        info.si_addr  = NULL;
        info.si_status = 0;
        signal_send_info(parent->pid, SIGCHLD, &info);
    }
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = 0;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);
    scheduler_yield();
    /* should never reach here */
    for (;;) __asm__ volatile("hlt");
}

void process_exit_code(int code) {
    /* Reparent orphans to init (PID 1) */
    uint32_t my_pid = current_process->pid;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (process_table[i].state != PROCESS_UNUSED &&
            process_table[i].parent_pid == my_pid &&
            process_table[i].pid != my_pid) {
            process_table[i].parent_pid = 1;
        }
    }
    current_process->state = PROCESS_ZOMBIE;
    current_process->exit_code = code;
    scheduler_remove(current_process);
    process_wake_waiter(current_process->pid);

    /* CLONE_CHILD_CLEARTID: write 0 to userspace CTID pointer and futex-wake */
    if (current_process->ctid_ptr && current_process->is_user) {
        volatile uint32_t *ctid = (volatile uint32_t *)current_process->ctid_ptr;
        *ctid = 0;
    }
    /* Send SIGCHLD to parent with siginfo */
    struct process *parent = process_get_by_pid(current_process->parent_pid);
    if (parent) {
        struct siginfo info;
        info.si_signo = SIGCHLD;
        info.si_errno = 0;
        info.si_code  = CLD_EXITED;
        info.si_pid   = current_process->pid;
        info.si_uid   = current_process->uid;
        info.si_addr  = NULL;
        info.si_status = code;
        signal_send_info(parent->pid, SIGCHLD, &info);
    }
    scheduler_yield();
    for (;;) __asm__ volatile("hlt");
}

/* ── O_CLOEXEC support ───────────────────────────────────────────────── */

/* Close all file descriptors with FD_CLOEXEC flag set */
void process_exec_close_cloexec(void) {
    struct process *cur = process_get_current();
    if (!cur) return;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (cur->fd_table[i].used && (cur->fd_table[i].flags & FD_CLOEXEC)) {
            cur->fd_table[i].used = 0;
            cur->fd_table[i].offset = 0;
            cur->fd_table[i].path[0] = '\0';
            cur->fd_table[i].flags = 0;
        }
    }
}

struct process *process_get_current(void) {
    struct process *proc = get_current_process();
    if (!proc) return current_process;
    return proc;
}

/* ── Capability bounding set ─────────────────────────────────────────── */

void cap_bset_drop(uint32_t cap) {
    struct process *p = process_get_current();
    if (!p || cap >= PROCESS_SYSCALL_MAX) return;
    int word = cap / 64;
    int bit  = cap % 64;
    p->cap_bset[word] &= ~(1ULL << bit);
}

int cap_bset_has(uint32_t cap) {
    struct process *p = process_get_current();
    if (!p || cap >= PROCESS_SYSCALL_MAX) return 0;
    int word = cap / 64;
    int bit  = cap % 64;
    return (p->cap_bset[word] & (1ULL << bit)) ? 1 : 0;
}

void cap_bset_init(struct process *proc) {
    if (!proc) return;
    /* Initialize bounding set to all ones (all caps allowed by default) */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++)
        proc->cap_bset[i] = ~0ULL;
}

/* ── Process credential API ─────────────────────────────────── */

int process_get_cred(uint32_t pid, uint32_t *uid, uint32_t *gid,
                     uint32_t *euid, uint32_t *egid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;
    if (uid)  *uid  = p->uid;
    if (gid)  *gid  = p->gid;
    if (euid) *euid = p->euid;
    if (egid) *egid = p->egid;
    return 0;
}

int process_set_cred(uint32_t pid, uint32_t uid, uint32_t gid,
                     uint32_t euid, uint32_t egid) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;
    p->uid  = uid;
    p->gid  = gid;
    p->euid = euid;
    p->egid = egid;
    return 0;
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
    if (alloc_guarded_kernel_stack(child) < 0) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -1;
    }
    child->state = PROCESS_READY;

    /* Clone user address space if process has one */
    if (parent->pml4) {
        child->pml4 = vmm_clone_user_pml4(parent->pml4);
        if (!child->pml4) {
            free_guarded_kernel_stack(child);
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
    if (alloc_guarded_kernel_stack(child) < 0) {
        child->state = PROCESS_UNUSED;
        __asm__ volatile("sti");
        return -1;
    }

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
                free_guarded_kernel_stack(child);
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

/* Check if the caller process can see (access) the target process.
 * Returns 1 if visible, 0 if not. */
int process_can_see(const struct process *caller, const struct process *target) {
    if (!caller || !target) return 0;
    if (caller == target) return 1;                /* self */
    if (caller->euid == 0) return 1;               /* root sees all */
    if (caller->euid == target->euid) return 1;    /* same uid */
    if (target->parent_pid == caller->pid) return 1; /* caller is parent */
    return 0;
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
        free_guarded_kernel_stack(proc);
    }
    /* Free user page tables (fixes leak) */
    if (proc->is_user && proc->pml4) {
        vmm_destroy_user_pml4(proc->pml4);
        proc->pml4 = NULL;
    }
    proc->state = PROCESS_UNUSED;
    if (proc->pid) {
        free_pid(proc->pid);
    }
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

/* ── Per-CPU kthread API ────────────────────────────────────── */

struct process *kthread_create(void (*entry)(void *arg), void *arg,
                                const char *name) {
    return kthread_create_on_cpu(entry, arg, name, -1);
}

struct process *kthread_create_on_cpu(void (*entry)(void *arg), void *arg,
                                       const char *name, int cpu_id) {
    struct process *proc = process_create((void (*)(void))entry, name);
    if (!proc) return NULL;

    proc->kthread_arg = arg;
    if (cpu_id >= 0 && cpu_id < SMP_MAX_CPUS)
        proc->cpu_affinity = (uint8_t)(1 << cpu_id);
    else
        proc->cpu_affinity = 0; /* any CPU */

    return proc;
}

/* ── process_is_kthread / process_set_user_process ──────────── */

int process_is_kthread(struct process *proc) {
    if (!proc) return 0;
    return (proc->is_user == 0 && proc->pid > 0);
}

int process_set_user_process(uint64_t entry, uint64_t stack, uint64_t *pml4) {
    struct process *proc = process_get_current();
    if (!proc) return -1;

    /* Can only convert kernel threads to user processes */
    if (proc->is_user) return -1;  /* already a user process */

    proc->is_user = 1;
    proc->user_entry = entry;
    proc->user_rsp = stack;
    proc->pml4 = pml4;

    /* Update capability profile for user execution */
    process_set_cap_profile(proc, PROCESS_CAP_PROFILE_USER_DEFAULT);

    return 0;
}
