#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"

#define SI_USER     0
#define SI_KERNEL   1
#define SI_QUEUE    2
#define SI_TKILL    3

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

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

typedef void (*signal_handler_t)(int signum);

#define SIG_MAX  32
#define SIG_DFL  ((signal_handler_t)0)  /* Default action */
#define SIG_IGN  ((signal_handler_t)1)  /* Ignore */

/* Send signal to process by pid; 0 = success, -1 = not found */
int signal_send(uint32_t pid, int signum);
int signal_send_group(uint32_t pgid, int signum);

/* Extended signal send with siginfo */
int signal_send_info(uint32_t pid, int signum, struct siginfo *info);

/* Check and deliver any pending signals for the current process.
 * Called by scheduler before returning to a READY process. */
void signal_check(void);

/* Register a signal handler for the current process */
void signal_register(int signum, signal_handler_t handler);

/* Mask/unmask signals for the current process (1-bit per signal number) */
void signal_mask(uint32_t sigmask);
void signal_unmask(uint32_t sigmask);

#endif
