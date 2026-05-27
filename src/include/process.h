#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#define PROCESS_MAX 256
#define KERNEL_STACK_SIZE (128 * 1024) /* 128 KB — cc parser + network call chains */
#define USER_STACK_SIZE   (64 * 1024)  /* 64 KB user stack */
#define PROCESS_SIG_MAX 32
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
struct process_fd {
    char     path[64];
    uint32_t offset;
    int      used;
};



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
    struct cpu_context *context;
    struct process *next;
    const char *name;
    /* Signal state */
    uint32_t pending_signals;               /* bitmask of pending signals */
    uint32_t sig_mask;                      /* bitmask of blocked (masked) signals */
    signal_handler_t sig_handlers[PROCESS_SIG_MAX]; /* per-signal handler */
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
    uint64_t syscall_caps[PROCESS_SYSCALL_CAP_WORDS];
    char     cwd[64];         /* current working directory */
    /* Waitpid: non-spinning wait for child */
    uint32_t wait_for_pid;    /* PID we are blocked waiting on (0 = none) */
    /* Scheduler: time-slice accounting */
    uint16_t ticks_remaining; /* ticks left in current quantum */
    uint64_t last_run_tick;   /* timer tick when process last ran (for aging) */
    /* Per-process file descriptor table */
    struct process_fd fd_table[PROCESS_FD_MAX];
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
void process_set_current(struct process *proc);

#endif
