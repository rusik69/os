#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "signal.h"

/* Resource limits (local defines since we can't rely on syscall.h include order) */
#define _RLIMIT_NLIMITS 14

#define PROCESS_MAX 256
#define KERNEL_STACK_SIZE (128 * 1024) /* 128 KB — cc parser + network call chains */
#define KERNEL_STACK_PAGES ((KERNEL_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)
#define KERNEL_STACK_TOTAL_PAGES (KERNEL_STACK_PAGES + 1) /* +1 guard page */
#define KERNEL_STACK_TOTAL_SIZE (KERNEL_STACK_TOTAL_PAGES * PAGE_SIZE)
#define USER_STACK_SIZE   (64 * 1024)  /* 64 KB user stack */
#define PROCESS_SIG_MAX 65
#define PROCESS_SYSCALL_MAX 256
#define PROCESS_SYSCALL_CAP_WORDS (PROCESS_SYSCALL_MAX / 64)
#define PROCESS_FD_MAX 16

/* User-space virtual addresses (canonical lower-half) */
#define USER_STACK_TOP    0x00007FFFFFFFE000ULL  /* top of user stack region */
#define USER_CODE_BASE    0x0000000000400000ULL  /* where ELF code is loaded */

enum process_state {
    PROCESS_UNUSED = 0,
    PROCESS_READY,
    PROCESS_RUNNING,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE,
};

struct cpu_context {
    uint64_t r15, r14, r13, r12;
    uint64_t rbx, rbp;
    uint64_t rflags;
    uint64_t rip;
} __attribute__((packed));

typedef void (*signal_handler_t)(int signum);

/* Per-process file descriptor table entry */
#define FD_CLOEXEC 1

struct process_fd {
    char     path[64];
    uint32_t offset;
    int      used;
    uint8_t  flags;       /* FD_CLOEXEC etc. */
};

/* Per-process interval timer (setitimer) */
#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2
#define ITIMER_MAX     3

struct itimerval {
    uint64_t it_interval;  /* ticks between deliveries */
    uint64_t it_value;     /* ticks until next delivery */
};

/* Clone flags (subset of Linux CLONE_*) */
#define CLONE_VM            0x00000100
#define CLONE_FILES         0x00000400
#define CLONE_SIGHAND       0x00000800
#define CLONE_THREAD        0x00010000
#define CLONE_CHILD_SETTID  0x01000000
#define CLONE_CHILD_CLEARTID 0x02000000
#define CLONE_SETTLS        0x00080000



enum process_cap_profile {
    PROCESS_CAP_PROFILE_NONE = 0,
    PROCESS_CAP_PROFILE_USER_DEFAULT,
    PROCESS_CAP_PROFILE_USER_TRUSTED,
};

struct process {
    uint32_t pid;
    enum process_state state;
    uint64_t kernel_stack;
    uint64_t stack_top;
    uint64_t guard_page;           /* VMA of unmapped guard page (0 if none) */
    struct cpu_context *context;
    struct process *next;
    const char *name;
    /* Signal state */
    uint64_t pending_signals;               /* bitmask of pending signals */
    uint64_t sig_mask;                      /* bitmask of blocked (masked) signals */
    signal_handler_t sig_handlers[PROCESS_SIG_MAX]; /* per-signal handler */
    struct siginfo sig_info[PROCESS_SIG_MAX]; /* most recent siginfo per signal */
    /* Ring 3 support */
    int      is_user;         /* 1 = runs in ring 3, 0 = kernel thread */
    uint64_t user_entry;      /* ring 3 entry point (ELF e_entry) */
    uint64_t user_rsp;        /* ring 3 stack pointer */
    uint64_t *pml4;           /* per-process page table (NULL = use kernel_pml4) */
    /* Multitasking */
    uint32_t parent_pid;      /* parent process PID */
    uint32_t pgid;            /* process group ID */
    uint32_t sid;             /* session ID */
    int      exit_code;       /* exit code when ZOMBIE */
    uint64_t sleep_until;     /* tick count to wake up (0 = not sleeping) */
    int      is_background;   /* 1 = launched with & */
    int      is_suspended;    /* 1 = stopped by job-control signal */
    int      on_queue;        /* 1 = currently on a scheduler run queue */
    uint8_t  cap_profile;     /* enum process_cap_profile */
    uint8_t  priority;        /* scheduler priority: 0=high .. 3=low */
    uint8_t  cpu_affinity;    /* bitmask of allowed CPUs (0 = all) */
    uint32_t uid;             /* user ID */
    uint32_t gid;             /* group ID */
    uint32_t euid;            /* effective user ID */
    uint32_t egid;            /* effective group ID */
    uint16_t umask;           /* file creation mask */
    uint64_t syscall_caps[PROCESS_SYSCALL_CAP_WORDS];
    char     cwd[64];         /* current working directory */
    /* Waitpid: non-spinning wait for child */
    uint32_t wait_for_pid;    /* PID we are blocked waiting on (0 = none) */
    /* Scheduler: time-slice accounting */
    uint16_t ticks_remaining; /* ticks left in current quantum */
    uint64_t last_run_tick;   /* timer tick when process last ran (for aging) */
    uint8_t  base_priority;    /* original priority before PI boost (restored on unlock) */
    /* Per-process file descriptor table */
    struct process_fd fd_table[PROCESS_FD_MAX];
    /* Thread group ID (same as pid for leader, same as leader for threads) */
    uint32_t tgid;
    /* SetChildTID / ClearChildTID userspace pointers for thread teardown */
    void *ctid_ptr;
    /* Per-CPU kthread argument */
    void *kthread_arg;
    /* Per-process interval timers */
    struct itimerval itimers[ITIMER_MAX];
    /* Resource limits (rlimit) */
    uint64_t rlim_cur[_RLIMIT_NLIMITS];
    uint64_t rlim_max[_RLIMIT_NLIMITS];
    /* Scheduling policy: SCHED_OTHER, SCHED_FIFO, SCHED_RR */
    uint8_t  sched_policy;
    /* Alternate signal stack */
    void    *alt_stack_sp;     /* ss_sp */
    uint64_t alt_stack_size;   /* ss_size */
    int      alt_stack_flags;  /* ss_flags: SS_DISABLE=2 */
    /* Personality (exec domain) */
    uint64_t personality;
    /* Core dump enabled? */
    int      coredump_enabled;
    /* Process name (for prctl PR_SET_NAME) */
    char     proc_comm[16];
};

void process_init(void);
struct process *process_create(void (*entry)(void), const char *name);
struct process *process_create_user(uint64_t entry, uint64_t user_rsp,
                                    uint64_t *pml4, const char *name);
void process_exit(void);
void process_exit_code(int code);
struct process *process_get_current(void);
struct process *process_get_by_pid(uint32_t pid);
struct process *process_get_table(void);
int process_can_see(const struct process *caller, const struct process *target);
int  process_waitpid(uint32_t pid, int *status);
void process_sleep_ticks(uint64_t ticks);
void process_reap_zombies(void);
void process_cleanup(struct process *proc);
void process_caps_clear_all(struct process *proc);
void process_caps_allow(struct process *proc, uint32_t num);
void process_caps_allow_all(struct process *proc);
int process_caps_has(const struct process *proc, uint32_t num);
int process_set_cap_profile(struct process *proc, enum process_cap_profile profile);
int process_fork(void); /* fork current process, returns child PID, child starts in fork_child_entry */
int process_clone(struct process *parent, uint64_t flags, void *child_stack,
                  uint64_t user_rip, uint64_t user_rflags);
void process_set_current(struct process *proc);

/* ── Per-CPU kthread API ────────────────────────────────────── */

struct process *kthread_create(void (*entry)(void *arg), void *arg,
                                const char *name);
struct process *kthread_create_on_cpu(void (*entry)(void *arg), void *arg,
                                       const char *name, int cpu_id);

/* Per-process interval timer tick (called from timer interrupt) */
void process_timer_tick(void);

#endif
