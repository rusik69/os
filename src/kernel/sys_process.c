/*
 * Linux-compatible process & signal syscalls.
 *
 * Provides the rt_sigaction, rt_sigprocmask, rt_sigreturn, kill, tkill,
 * tgkill, clone, clone3, wait4, waitid, exit_group, gettid, and
 * set_tid_address syscalls that match the Linux x86-64 ABI conventions.
 *
 * Each function returns (uint64_t)(int64_t)-errno on error, or a
 * non-negative value on success.
 */
#include "syscall.h"
#include "process.h"
#include "signal.h"
#include "module.h"
#include "errno.h"
#include "uaccess.h"
#include "spinlock.h"
#include "printf.h"
#include "signal_libc.h"    /* for struct sigaction (userspace ABI) */

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Linux-compatible process & signal syscalls");
MODULE_AUTHOR("Ruslan Gustomiasov");

/* ── sys_rt_sigaction — register/query signal handlers ────────────
 *
 *   int rt_sigaction(int signum, const struct sigaction *act,
 *                     struct sigaction *oldact, size_t sigsetsize);
 *
 * If act is non-NULL, installs a new handler for signum.
 * If oldact is non-NULL, returns the previous handler.
 * sigsetsize must be sizeof(sigset_t) (8 bytes on x86-64).
 *
 * Returns 0 on success, -errno on error.
 */
uint64_t sys_rt_sigaction(uint64_t signum, uint64_t act_addr,
                          uint64_t oldact_addr, uint64_t sigsetsize)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Validate sigsetsize — must match kernel's sigset_t size */
    if (sigsetsize != sizeof(uint64_t))
        return (uint64_t)(int64_t)-EINVAL;

    /* Validate signal number range */
    if (signum <= 0 || signum >= SIG_MAX)
        return (uint64_t)(int64_t)-EINVAL;

    /* SIGKILL and SIGSTOP cannot have their dispositions changed */
    if (signum == SIGKILL || signum == SIGSTOP)
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    /* If oldact is requested, copy the current handler state out */
    if (oldact_addr) {
        struct sigaction old;
        old.sa_handler = (void (*)(int))(uintptr_t)p->sig_handlers[signum];
        old.sa_sigaction = NULL;  /* sa_handler and sa_sigaction share the same slot in Linux */
        old.sa_flags   = (int)p->sig_flags[signum];
        old.sa_restorer = NULL;  /* not tracked per-signal */
        old.sa_mask.__bits[0] = p->sig_mask;

        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (copy_to_user(oldact_addr, &old, sizeof(old)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    }

    /* If act is provided, install the new handler */
    if (act_addr) {
        struct sigaction new_act;
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);

        if (copy_from_user(&new_act, act_addr, sizeof(new_act)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        /* Validate that the handler address is sane.
         * For user processes, SIG_DFL (0) and SIG_IGN (1) are the only
         * special values — anything else must be a valid userspace address. */
        signal_handler_t handler = (signal_handler_t)(uintptr_t)new_act.sa_handler;
        if (p->is_user &&
            handler != SIG_DFL && handler != SIG_IGN &&
            (uint64_t)(uintptr_t)handler >= USER_VADDR_MAX)
            return (uint64_t)(int64_t)-EINVAL;

        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

        p->sig_handlers[signum] = handler;
        p->sig_flags[signum]    = (uint32_t)(new_act.sa_flags & 0xFFFFFFFFU);

        /* sa_mask: set of signals to block during handler execution.
         * Stored in sig_info for signal_check() to apply at delivery time.
         * Currently the kernel uses p->sig_mask (global mask); per-signal
         * sa_mask application during delivery is a future enhancement.
         * TODO: apply sa_mask during signal_check() delivery. */
        (void)new_act.sa_mask;

        /* SA_NODEFER: normally the signal is masked while its handler runs.
         * If set, don't mask it. This is handled at signal delivery time.
         * SA_RESETHAND: reset handler to SIG_DFL after first delivery.
         * SA_RESTART: restart interrupted syscalls.
         * SA_ONSTACK: use alternate signal stack.
         * These flags are stored in sig_flags for use by signal delivery. */
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    return 0;
}
