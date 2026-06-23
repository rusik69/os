#ifndef SIGNAL_LIBC_H
#define SIGNAL_LIBC_H

#include "types.h"
#include "signal.h"       /* for struct siginfo */

/*
 * ── POSIX signal types and functions ─────────────────────────────────
 *
 * This header provides the standard POSIX signal API for userspace
 * programs compiled with this kernel's userspace environment.
 *
 * The kernel's internal signal.h uses a different naming convention;
 * this header is the userspace-facing interface.
 */

/* Signal set type — for sigprocmask, sigpending, sigsuspend, etc. */
typedef struct {
    uint64_t __bits[1];  /* enough for 64 signals */
} sigset_t;

/* POSIX sigaction structure */
struct sigaction {
    void     (*sa_handler)(int);      /* signal handler */
    void     (*sa_sigaction)(int, struct siginfo *, void *); /* RT handler */
    sigset_t  sa_mask;                /* signals to block during handler */
    int       sa_flags;               /* SA_* flags */
    void     (*sa_restorer)(void);    /* not used */
};

/* sa_flags values */
#ifndef SA_NOCLDSTOP
#define SA_NOCLDSTOP  0x00000001
#endif

/* Compile-time ABI assertion: struct sigaction must match userspace expectations */
_Static_assert(sizeof(struct sigaction) == 40, "struct sigaction size mismatch");
#ifndef SA_NOCLDWAIT
#define SA_NOCLDWAIT  0x00000002
#endif
#ifndef SA_SIGINFO
#define SA_SIGINFO    0x00000004
#endif
#ifndef SA_ONSTACK
#define SA_ONSTACK    0x08000000
#endif
#ifndef SA_RESTART
#define SA_RESTART    0x10000000
#endif
#ifndef SA_NODEFER
#define SA_NODEFER    0x40000000
#endif
#ifndef SA_RESETHAND
#define SA_RESETHAND  0x80000000
#endif

/* sigprocmask how values */
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

/* ── Signal set manipulation (POSIX) ─────────────────────────────────── */

/* Initialize empty signal set */
static inline void sigemptyset(sigset_t *set) {
    set->__bits[0] = 0;
}

/* Initialize full signal set (all signals) */
static inline void sigfillset(sigset_t *set) {
    set->__bits[0] = ~0ULL;
}

/* Add a signal to the set */
static inline int sigaddset(sigset_t *set, int signum) {
    if (signum < 1 || signum > 64) return -1;
    set->__bits[0] |= (1ULL << (uint64_t)signum);
    return 0;
}

/* Remove a signal from the set */
static inline int sigdelset(sigset_t *set, int signum) {
    if (signum < 1 || signum > 64) return -1;
    set->__bits[0] &= ~(1ULL << (uint64_t)signum);
    return 0;
}

/* Test whether a signal is in the set */
static inline int sigismember(const sigset_t *set, int signum) {
    if (signum < 1 || signum > 64) return 0;
    return (set->__bits[0] & (1ULL << (uint64_t)signum)) != 0;
}

/* ── POSIX signal functions ──────────────────────────────────────────── */

/* Simplified signal() — register a signal handler */
void (*signal(int signum, void (*handler)(int)))(int);

/* POSIX sigaction() — register action for a signal */
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);

/* Examine/change the signal mask */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

/* Atomically replace mask and wait for a signal */
int sigsuspend(const sigset_t *mask);

/* Get pending signals */
int sigpending(sigset_t *set);

/* Send signal to process */
int kill(uint32_t pid, int sig);

/* Wait for a signal */
int sigwaitinfo(const sigset_t *set, struct siginfo *info);

/* Raise a signal to the current process */
int raise(int sig);

#endif /* SIGNAL_LIBC_H */
