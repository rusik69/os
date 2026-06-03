#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"
#include "signal.h"
#include "pelt.h"

/* Resource limits (local defines since we can't rely on syscall.h include order) */
#define _RLIMIT_NLIMITS 14

#define PROCESS_MAX 256
#define KERNEL_STACK_SIZE (128 * 1024) /* 128 KB — cc parser + network call chains */
#define KERNEL_STACK_PAGES ((KERNEL_STACK_SIZE + PAGE_SIZE - 1) / PAGE_SIZE)
#define KERNEL_STACK_TOTAL_PAGES (KERNEL_STACK_PAGES + 1) /* +1 guard page */
#define KERNEL_STACK_TOTAL_SIZE (KERNEL_STACK_TOTAL_PAGES * PAGE_SIZE)
#define USER_STACK_SIZE   (64 * 1024)  /* 64 KB user stack */
#define PROCESS_SIG_MAX 65
#define PROCESS_SYSCALL_MAX 512
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
#define FD_CLOEXEC  1
#define FD_TMPFILE  2  /* O_TMPFILE — unlink on close */

struct process_fd {
    char     path[64];
    uint32_t offset;
    int      used;
    uint8_t  flags;       /* FD_CLOEXEC etc. */
    uint32_t sigio_pid;   /* PID to receive SIGIO (0 = none) */
    int      advice;      /* POSIX_FADV_* hint (-1 = unset) */
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
    /* seccomp mode and filter */
    int      seccomp_mode;
    struct seccomp_filter *seccomp_filter;  /* NULL if no filter installed */
    /* ── CPU time accounting ─────────────────────────────────── */
    uint64_t utime_ticks;    /* ticks spent in user mode */
    uint64_t stime_ticks;    /* ticks spent in kernel mode (syscalls + IRQs) */
    uint64_t start_time_tick; /* tick when process was created */
    uint64_t nvcsw;           /* voluntary context switches (yield, sleep) */
    uint64_t nivcsw;          /* involuntary context switches (preemption) */
    uint64_t minflt;          /* minor page faults (no disk I/O) */
    uint64_t majflt;          /* major page faults (disk I/O required) */
    /* Heap tracking for brk */
    uint64_t heap_end;        /* current end of data segment for brk() */
    /* Capability bounding set */
    uint64_t cap_bset[PROCESS_SYSCALL_CAP_WORDS];
    /* no_new_privs flag */
    int no_new_privs;
    /* Landlock ruleset IDs (-1 per slot = unrestricted).
     * Supports stacking up to LANDLOCK_MAX_RULESETS_PER_PROC (4) rulesets. */
    int landlock_ruleset_ids[4];
    /* Securebits flags */
    uint8_t securebits;
    /* Capability sets (Linux-style) */
    uint64_t cap_effective[PROCESS_SYSCALL_CAP_WORDS];
    uint64_t cap_permitted[PROCESS_SYSCALL_CAP_WORDS];
    uint64_t cap_inheritable[PROCESS_SYSCALL_CAP_WORDS];
    /* Parent death signal (PR_SET_PDEATHSIG) */
    int pdeath_signal;
    /* membarrier registration flags (MEMBARRIER_PRIVATE_EXPEDITED, etc.) */
    int membarrier_flags;
    /* VM lock flags (mlockall) */
    int vm_locked_flags;  /* bitmask: MCL_CURRENT=1, MCL_FUTURE=2 */
    /* ── CFS vruntime (nanosecond-scale) ───────────────────── */
    uint64_t vruntime;           /* virtual runtime for CFS */
    uint64_t sched_weight;       /* scheduling weight (default 1024) */
    int      nice;               /* POSIX nice value (-20..+19, default 0) */
    int      sched_autogroup_id; /* autogroup ID (-1 = none) */
    /* I/O priority (ioprio) for block layer ordering */
    uint16_t ioprio;             /* I/O priority value (default: IOPRIO_DEFAULT) */
    /* ── SCHED_DEADLINE parameters ─────────────────────────── */
    uint64_t dl_runtime;         /* worst-case execution time per period (ns) */
    uint64_t dl_deadline;        /* relative deadline (ns) */
    uint64_t dl_period;          /* period (ns) */
    uint64_t dl_deadline_abs;    /* absolute deadline for current period (ns) */
    uint64_t dl_runtime_remaining; /* remaining budget (ns) */
    uint64_t dl_period_start;    /* start time of current period (ns) */
    int      dl_throttled;       /* 1 = budget exhausted before deadline */
    int      dl_active;          /* 1 = deadline scheduling active */
    /* ── Resource tracking ──────────────────────────────────── */
    uint64_t cpu_user;           /* user CPU time (ticks) */
    uint64_t cpu_system;         /* system CPU time (ticks) */
    uint64_t max_rss;            /* max resident set size (pages) */
    uint64_t page_faults;        /* total page faults */
    uint64_t signals_received;   /* total signals received */
    uint64_t context_switches;   /* total context switches */
    /* ── Stack usage tracking ───────────────────────────────── */
    uint64_t stack_watermark;    /* lowest RSP observed */
    /* ── File descriptor limits ──────────────────────────────── */
    uint64_t file_max;    /* max open files (rlimit NOFILE) */
    /* ── Exec credential security ────────────────────────────── */
    int dumpable;         /* SUID_DUMP_USER=1 (default), SUID_DUMP_DISABLE=0 */
    /* ── PELT load tracking ──────────────────────────────────── */
    struct pelt_state pelt;  /* per-entity load tracking state */
    /* ── NUMA home node — processor affinity node for memory and scheduling ─ */
    int cpu_cgroup_id;
    /* ── NUMA home node — preferred NUMA node for scheduling this task ── */
    int home_node;         /* NUMA node ID (0 = default, -1 = not assigned) */
    /* ── Held mutex tracking for Priority Inheritance ─────────────── */
#define PROCESS_MAX_HELD_MUTEXES 4
    int held_mutex_count;          /* number of mutexes currently held */
    int held_mutex_ids[PROCESS_MAX_HELD_MUTEXES]; /* mutex IDs held by this process */
    /* ── Executable path (for /proc/self/exe) ────────────────────── */
    char exe_path[256];
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
uint32_t process_get_count(void);

/* Process credential API */
int process_get_cred(uint32_t pid, uint32_t *uid, uint32_t *gid,
                     uint32_t *euid, uint32_t *egid);
int process_set_cred(uint32_t pid, uint32_t uid, uint32_t gid,
                     uint32_t euid, uint32_t egid);

/* Dumpable flag API */
int process_get_dumpable(struct process *proc);
int process_set_dumpable(struct process *proc, int val);

/* Exec credential security — call during execve */
void process_exec_cred_security(void);

/* Close all file descriptors with FD_CLOEXEC set */
void process_exec_close_cloexec(void);

/* Apply securebits and capabilities on exec */
void process_exec_caps(void);

/* Capability bounding set API */
void cap_bset_drop(uint32_t cap);
int  cap_bset_has(uint32_t cap);
void cap_bset_init(struct process *proc);

/* Securebits */
#define SECBIT_KEEP_CAPS              (1 << 0)
#define SECBIT_NO_SETUID_FIXUP        (1 << 1)
#define SECBIT_KEEP_CAPS_LOCKED       (1 << 2)
#define SECBIT_NO_SETUID_FIXUP_LOCKED (1 << 3)
#define SECBIT_NOROOT                 (1 << 4)  /* Don't grant root privileges even if UID 0 */
#define SECBIT_NOROOT_LOCKED          (1 << 5)
#define SECBIT_ALLOWED_MASK           (SECBIT_KEEP_CAPS | SECBIT_NO_SETUID_FIXUP | SECBIT_NOROOT)
#define SECBIT_LOCKED_MASK            (SECBIT_KEEP_CAPS_LOCKED | SECBIT_NO_SETUID_FIXUP_LOCKED | SECBIT_NOROOT_LOCKED)
int  securebits_get(struct process *proc);
int  securebits_set(struct process *proc, uint8_t bits);

/* ── Per-CPU kthread API ────────────────────────────────────── */

struct process *kthread_create(void (*entry)(void *arg), void *arg,
                                const char *name);
struct process *kthread_create_on_cpu(void (*entry)(void *arg), void *arg,
                                       const char *name, int cpu_id);

/* ── Thread management (pthread support) ───────────────────── */
int  process_thread_create(void *(*start_routine)(void *), void *arg);
int  process_thread_join(int thread_pid, void **retval);
void process_thread_exit(void *retval) __attribute__((noreturn));
void thread_info_init(void);

/* Per-process interval timer tick (called from timer interrupt) */
void process_timer_tick(int was_user);

/* ── User process conversion / query ───────────────────────── */
int process_is_kthread(struct process *proc);
int process_set_user_process(uint64_t entry, uint64_t stack, uint64_t *pml4);

#endif
