#ifndef PROCESS_H
#define PROCESS_H

#include "types.h"

#define PROCESS_MAX 64
#define KERNEL_STACK_SIZE (32 * 1024)  /* 32 KB — network TX chain uses ~5KB of stack */
#define PROCESS_SIG_MAX 32

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
    signal_handler_t sig_handlers[PROCESS_SIG_MAX]; /* per-signal handler */
};

void process_init(void);
struct process *process_create(void (*entry)(void), const char *name);
void process_exit(void);
struct process *process_get_current(void);
struct process *process_get_by_pid(uint32_t pid);
struct process *process_get_table(void);

#endif
