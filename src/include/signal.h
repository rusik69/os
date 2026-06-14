#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"

#define SI_USER     0
#define SI_KERNEL   1
#define SI_QUEUE    2
#define SI_TKILL    3

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

/* SIGBUS si_code values */
#define BUS_ADRERR   3  /* non-existent physical address */

/* Signal info structure (subset of siginfo_t) */
struct siginfo {
    int      si_signo;    /* signal number */
    int      si_errno;    /* errno (unused) */
    int      si_code;     /* signal code */
    uint32_t si_pid;      /* sending process */
    uint32_t si_uid;      /* real user ID */
    void    *si_addr;     /* fault address (for SIGSEGV/SIGBUS) */
    int      si_status;   /* exit value (for SIGCHLD) */
};

/* Signal numbers */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGSYS    31

/* Real-time signals */
#define SIGRTMIN  32
#define SIGRTMAX  64

/* SIGCHLD si_code values */
#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_TRAPPED   4
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

typedef void (*signal_handler_t)(int signum);

#define SIG_MAX  65  /* signals 1-64 */
#define SIG_DFL  ((signal_handler_t)0)  /* Default action */
#define SIG_IGN  ((signal_handler_t)1)  /* Ignore */

/* Alternate signal stack (sigaltstack) */
typedef struct {
    void   *ss_sp;       /* stack base/pointer */
    int     ss_flags;    /* SS_DISABLE, SS_ONSTACK */
    size_t  ss_size;     /* stack size */
} stack_t;

#define SS_DISABLE  2
#define SS_ONSTACK  1

/* sigaltstack flags for syscall */
#define SA_ONSTACK     0x08000000
#define SA_SIGINFO     0x00000004
#define SA_RESTART     0x10000000
#define SA_NODEFER     0x40000000
#define SA_RESETHAND   0x80000000

/* Send signal to process by pid; 0 = success, -1 = not found */
int signal_send(uint32_t pid, int signum);
int signal_send_group(uint32_t pgid, int signum);
int signal_send_pgid(uint32_t pgid, int signum);

/* Extended signal send with siginfo */
int signal_send_info(uint32_t pid, int signum, struct siginfo *info);

/* Check and deliver any pending signals for the current process.
 * Called by scheduler before returning to a READY process. */
void signal_check(void);

/* Register a signal handler for the current process */
void signal_register(int signum, signal_handler_t handler);
void signal_register_flags(int signum, signal_handler_t handler, uint32_t flags);

/* Mask/unmask signals for the current process (1-bit per signal number) */
void signal_mask(uint64_t sigmask);
void signal_unmask(uint64_t sigmask);

/* Retrieve siginfo for a pending signal (returns NULL if none stored).
 * Caller should clear the info after use. */
struct process;
struct siginfo *signal_get_info(struct process *p, int signum);

#endif
