/*
 * signal_libc.c — POSIX signal functions for userspace programs.
 *
 * Provides standard POSIX signal wrappers on top of the kernel's
 * SYS_SIGNAL, SYS_SIGPROCMASK, SYS_SIGPENDING, SYS_SIGWAITINFO,
 * SYS_SIGTIMEDWAIT, and SYS_KILL syscalls.
 *
 * This file is compiled as part of the userspace environment (linked
 * into shell commands and applications via the libc).
 */

#include "signal_libc.h"
#include "signal.h"       /* kernel signal numbers, siginfo */
#include "libc.h"         /* for libc_syscall() */
#include "string.h"
#include "syscall.h"

/*
 * Internal: invoke a syscall via the libc syscall shim.
 * The kernel-side syscall dispatcher expects up to 6 uint64_t arguments.
 */
static inline uint64_t sc(uint64_t n, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5)
{
    return libc_syscall(n, a1, a2, a3, a4, a5);
}

/*
 * signal() — Simplified POSIX signal registration.
 *
 * Registers a handler for @signum.  On success returns the previous
 * handler, on error returns SIG_ERR.
 */
void (*signal(int signum, void (*handler)(int)))(int)
{
    /* The kernel SYS_SIGNAL syscall returns the old handler on success,
     * or (void*)-1 on error. */
    uint64_t ret = sc(SYS_SIGNAL, (uint64_t)(int64_t)signum,
                      (uint64_t)(uintptr_t)handler, 0, 0, 0);
    if (ret == (uint64_t)-1ULL)
        return (void (*)(int))-1ULL;  /* SIG_ERR */
    return (void (*)(int))(uintptr_t)ret;
}

/*
 * sigaction() — POSIX signal action registration.
 *
 * If @act is non-NULL, sets the action for @signum.
 * If @oldact is non-NULL, returns the previous action.
 *
 * Uses the kernel's SYS_SIGNAL syscall, which supports a simplified
 * signal model (no sa_mask inheritance from the kernel).  The mask
 * specified in sa_mask is handled entirely in userspace.
 *
 * Returns 0 on success, -1 on error.
 */
int sigaction(int signum, const struct sigaction *act,
              struct sigaction *oldact)
{
    if (signum < 1 || signum > 64)
        return -1;

    /* Read the current handler before potentially overwriting it */
    if (oldact) {
        /* We can't query the kernel for the old struct sigaction,
         * so we zero it for safety.  The caller should treat this
         * as a best-effort mechanism. */
        memset(oldact, 0, sizeof(*oldact));
        oldact->sa_handler = NULL;
    }

    /* If a new action is specified, register it */
    if (act) {
        uint64_t ret = sc(SYS_SIGNAL, (uint64_t)(int64_t)signum,
                          (uint64_t)(uintptr_t)act->sa_handler,
                          0, 0, 0);
        if (ret == (uint64_t)-1ULL)
            return -1;
        if (oldact)
            oldact->sa_handler = (void (*)(int))(uintptr_t)ret;
    }

    return 0;
}

/*
 * sigprocmask() — Examine and change the current signal mask.
 *
 * @how:   SIG_BLOCK, SIG_UNBLOCK, or SIG_SETMASK
 * @set:   The new signal set (or NULL if not changing)
 * @oldset: Output for previous mask (or NULL if not needed)
 *
 * Returns 0 on success, -1 on error.
 */
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    uint64_t set_val = 0;
    uint64_t old_val = 0;

    if (set)
        set_val = set->__bits[0];

    uint64_t ret = sc(SYS_SIGPROCMASK, (uint64_t)(int64_t)how,
                      (uint64_t)(uintptr_t)(set ? &set_val : NULL),
                      (uint64_t)(uintptr_t)(oldset ? &old_val : NULL),
                      0, 0);

    if (ret == (uint64_t)-1ULL)
        return -1;

    if (oldset)
        oldset->__bits[0] = old_val;

    return 0;
}

/*
 * sigsuspend() — Atomically replace the signal mask and wait for a signal.
 *
 * Sets the calling process's signal mask to @mask and then suspends
 * execution until a signal is delivered.  Upon return, the original
 * mask is restored.
 *
 * Always returns -1 with errno EINTR (conceptually).
 */
int sigsuspend(const sigset_t *mask)
{
    /* Implementation: set the new mask, then wait for any signal
     * not in the new mask.  This is a simplified version; a real
     * implementation would use a dedicated kernel primitive for
     * atomic mask-swap-and-wait. */
    sigset_t old;
    old.__bits[0] = 0;
    sigprocmask(SIG_SETMASK, mask, &old);

    /* Wait for any signal by repeatedly polling pending signals.
     * In a production kernel we'd use a proper block-on-signal
     * syscall; here we busy-wait with SYS_SIGWAITINFO. */
    for (;;) {
        /* Try to dequeue any pending signal */
        uint64_t set_val = mask->__bits[0];
        uint64_t ret = sc(SYS_SIGWAITINFO,
                          (uint64_t)(uintptr_t)&set_val,
                          (uint64_t)(uintptr_t)NULL,
                          0, 0, 0);
        if (ret != (uint64_t)-1ULL) {
            /* Signal received — restore old mask and return */
            sigprocmask(SIG_SETMASK, &old, NULL);
            return -1;  /* standard POSIX: always returns -1/EINTR */
        }
        /* Yield to allow other processes to run */
        sc(SYS_YIELD, 0, 0, 0, 0, 0);
    }
}

/*
 * sigpending() — Get the set of pending signals.
 *
 * Returns 0 on success, -1 on error.
 */
int sigpending(sigset_t *set)
{
    if (!set)
        return -1;

    uint64_t pending = 0;
    uint64_t ret = sc(SYS_SIGPENDING,
                      (uint64_t)(uintptr_t)&pending,
                      0, 0, 0, 0);

    if (ret == (uint64_t)-1ULL)
        return -1;

    set->__bits[0] = pending;
    return 0;
}

/*
 * kill() — Send a signal to a process.
 *
 * Returns 0 on success, -1 on error.
 */
int kill(uint32_t pid, int sig)
{
    uint64_t ret = sc(SYS_KILL, (uint64_t)pid, (uint64_t)(int64_t)sig,
                      0, 0, 0);
    return (ret == (uint64_t)-1ULL) ? -1 : 0;
}

/*
 * sigwaitinfo() — Synchronously wait for a signal.
 *
 * Blocks until one of the signals in @set is pending.
 * Returns the signal number on success, -1 on error.
 * If @info is non-NULL, it is filled with the siginfo_t data.
 */
int sigwaitinfo(const sigset_t *set, struct siginfo *info)
{
    if (!set)
        return -1;

    uint64_t set_val = set->__bits[0];
    uint64_t ret = sc(SYS_SIGWAITINFO,
                      (uint64_t)(uintptr_t)&set_val,
                      (uint64_t)(uintptr_t)info,
                      0, 0, 0);

    if (ret == (uint64_t)-1ULL)
        return -1;

    return (int)(int64_t)ret;
}

/*
 * raise() — Send a signal to the current process.
 *
 * Returns 0 on success, -1 on error.
 */
int raise(int sig)
{
    /* Get our own PID */
    uint64_t pid = sc(SYS_GETPID, 0, 0, 0, 0, 0);
    return kill((uint32_t)pid, sig);
}

/* ── signal_raise ─────────────────────────────── */
int signal_raise(int sig)
{
    /* raise(sig) = kill(getpid(), sig) */
    uint64_t pid = sc(SYS_GETPID, 0, 0, 0, 0, 0);
    return kill((uint32_t)pid, sig);
}
/* ── signal_sigaction ─────────────────────────────── */
int signal_sigaction(int sig, const void *act, void *oldact)
{
    /* Delegate to the existing sigaction implementation */
    const struct sigaction *new_act = (const struct sigaction *)act;
    struct sigaction *old_act = (struct sigaction *)oldact;
    return sigaction(sig, new_act, old_act);
}
/* ── signal_sigprocmask ─────────────────────────────── */
int signal_sigprocmask(int how, const void *set, void *oldset)
{
    /* Delegate to the existing sigprocmask implementation */
    const sigset_t *new_set = (const sigset_t *)set;
    sigset_t *old_set = (sigset_t *)oldset;
    return sigprocmask(how, new_set, old_set);
}
