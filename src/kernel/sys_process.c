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
#include "signal_frame.h"   /* for ucontext_t, sigcontext */
#include "vmm.h"            /* for USER_VADDR_MAX */
#include "timer.h"          /* for TIMER_FREQ, NS_PER_TICK, timer_get_ticks */
#include "scheduler.h"      /* for scheduler_remove, scheduler_yield */

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

/* ── sys_rt_sigprocmask — examine/change signal mask ─────────────
 *
 *   int rt_sigprocmask(int how, const sigset_t *set,
 *                       sigset_t *oldset, size_t sigsetsize);
 *
 * Examine or change the signal mask of the calling thread.
 * how can be SIG_BLOCK (0), SIG_UNBLOCK (1), or SIG_SETMASK (2).
 * If set is non-NULL, apply the new mask according to how.
 * If oldset is non-NULL, the previous mask is stored there.
 * sigsetsize must be sizeof(sigset_t) (8 bytes on x86-64).
 *
 * Returns 0 on success, -errno on error.
 */
uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set_addr,
                            uint64_t oldset_addr, uint64_t sigsetsize)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Validate sigsetsize — must match kernel's sigset_t size */
    if (sigsetsize != sizeof(uint64_t))
        return (uint64_t)(int64_t)-EINVAL;

    /* Validate how parameter */
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK)
        return (uint64_t)(int64_t)-EINVAL;

    uint64_t __sig_flags;
    uint64_t old_mask;

    /* Snapshot the current mask atomically */
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    old_mask = p->sig_mask;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);

    /* Copy old mask to userspace */
    if (oldset_addr) {
        if (copy_to_user(oldset_addr, &old_mask, sizeof(old_mask)) < 0)
            return (uint64_t)(int64_t)-EFAULT;
    }

    /* Read new mask from userspace and apply */
    if (set_addr) {
        uint64_t new_mask = 0;
        if (copy_from_user(&new_mask, set_addr, sizeof(new_mask)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
        switch (how) {
        case SIG_BLOCK:
            p->sig_mask |= new_mask;
            break;
        case SIG_UNBLOCK:
            p->sig_mask &= ~new_mask;
            break;
        case SIG_SETMASK:
            p->sig_mask = new_mask;
            break;
        }
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    }

    return 0;
}

/* ── sys_rt_sigreturn — restore context from signal frame ─────────
 *
 *   int rt_sigreturn(void);
 *
 * Called by the signal handler trampoline (__restore_rt) after the
 * user-registered signal handler returns.  Reads the saved register
 * context and signal mask from the signal frame on the user stack,
 * restores them, and returns to the point where execution was
 * interrupted by the signal.
 *
 * Does NOT return a meaningful value to the caller — instead the
 * saved registers from the signal frame are injected into the kernel
 * stack frame so the syscall exit path returns to the interrupted
 * user code with the pre-signal register state.
 *
 * Returns rax from the saved context on success, -errno on error.
 */

/* syscall_entry_rsp_saved is set in syscall_asm.asm to the RSP after
 * all user registers are pushed onto the kernel stack.  It points to
 * the r15 slot — the base of the saved register frame. */
extern volatile uint64_t syscall_entry_rsp_saved;

uint64_t sys_rt_sigreturn(void)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /*
     * The saved user RSP is at index [8] in the kernel stack frame
     * (9th qword, offset +64 from the r15 base).
     *
     * At the time this syscall is invoked, the saved user RSP points
     * to the ucontext within the signal frame on the user stack.
     * The signal handler's ret instruction consumed the pretcode
     * return address, advancing RSP past it to the ucontext, and
     * the trampoline called rt_sigreturn without modifying RSP.
     */
    volatile uint64_t *saved_frame = (volatile uint64_t *)(uintptr_t)syscall_entry_rsp_saved;
    uint64_t user_rsp = saved_frame[8];

    /* Validate the ucontext address is in user space */
    if (user_rsp == 0 || user_rsp >= USER_VADDR_MAX)
        return (uint64_t)(int64_t)-EFAULT;

    /* Read ucontext from user stack */
    ucontext_t uc;
    if (copy_from_user(&uc, user_rsp, sizeof(uc)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Restore signal mask from the saved ucontext */
    {
        uint64_t __sig_flags;
        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
        p->sig_mask = uc.uc_sigmask;
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    }

    /* Overwrite the saved kernel stack frame with the signal context.
     *
     * The asm exit path (syscall_asm.asm) pops these values before
     * returning to userspace via sysret.  By overwriting them here,
     * the signal handler's context restore takes effect transparently.
     *
     * Layout (from saved_frame, which points to the r15 slot):
     *   [0] r15     -> popped into R15
     *   [1] r14     -> popped into R14
     *   [2] r13     -> popped into R13
     *   [3] r12     -> popped into R12
     *   [4] rbx     -> popped into RBX
     *   [5] rbp     -> popped into RBP
     *   [6] r11     -> popped into R11 -> sysret reads RFLAGS from R11
     *   [7] rcx     -> popped into RCX -> sysret reads RIP from RCX
     *   [8] user RSP -> popped into RSP
     */
    saved_frame[0] = uc.uc_mcontext.r15;
    saved_frame[1] = uc.uc_mcontext.r14;
    saved_frame[2] = uc.uc_mcontext.r13;
    saved_frame[3] = uc.uc_mcontext.r12;
    saved_frame[4] = uc.uc_mcontext.rbx;
    saved_frame[5] = uc.uc_mcontext.rbp;
    saved_frame[6] = uc.uc_mcontext.rflags;
    saved_frame[7] = uc.uc_mcontext.rip;
    saved_frame[8] = uc.uc_mcontext.rsp;

    /* Return rax from the saved context so the interrupted user code
     * sees the rax value it had before the signal interrupted it,
     * not the return value of the sigreturn syscall itself. */
    return uc.uc_mcontext.rax;
}

/* ── sys_rt_sigtimedwait — synchronously wait for signals ────────
 *
 *   int rt_sigtimedwait(const sigset_t *set, siginfo_t *info,
 *                        const struct timespec *timeout,
 *                        size_t sigsetsize);
 *
 * Suspends the calling thread until one of the signals in `set` is
 * pending.  If a signal in `set` is already pending at the time of
 * the call, it is consumed and returned immediately.
 *
 * If `info` is non-NULL, the siginfo_t associated with the signal
 * is returned there (if available, otherwise zeroed with si_signo
 * and si_code set).
 *
 * If `timeout` is NULL, wait indefinitely.
 * If `timeout` points to a zero-valued timespec {0, 0}, poll once
 * and return -EAGAIN if no signal is pending.
 *
 * Returns the signal number on success, -errno on error.
 */
uint64_t sys_rt_sigtimedwait(uint64_t set_addr, uint64_t info_addr,
                             uint64_t timeout_addr, uint64_t sigsetsize)
{
    struct process *p = process_get_current();
    if (!p)
        return (uint64_t)(int64_t)-ESRCH;

    /* Validate sigsetsize — must match kernel's sigset_t size */
    if (sigsetsize != sizeof(uint64_t))
        return (uint64_t)(int64_t)-EINVAL;

    /* Read the signal set from userspace */
    uint64_t sig_set = 0;
    if (copy_from_user(&sig_set, set_addr, sizeof(sig_set)) < 0)
        return (uint64_t)(int64_t)-EFAULT;

    /* Empty set is invalid */
    if (sig_set == 0)
        return (uint64_t)(int64_t)-EINVAL;

    /* Mask out signals that can't be waited on */
    sig_set &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    /* Parse timeout */
    int poll_mode = 0;         /* 1 = return -EAGAIN if no signal pending now */
    uint64_t timeout_abs = 0;  /* absolute tick deadline (0 = no timeout) */
    if (timeout_addr) {
        struct timespec ts;
        if (copy_from_user(&ts, timeout_addr, sizeof(ts)) < 0)
            return (uint64_t)(int64_t)-EFAULT;

        if (ts.tv_sec == 0 && ts.tv_nsec == 0) {
            poll_mode = 1;
        } else {
            /* Convert timespec to ticks, rounding up so we don't wake early */
            uint64_t timeout_ticks = ts.tv_sec * (uint64_t)TIMER_FREQ;
            timeout_ticks += (ts.tv_nsec + (uint64_t)NS_PER_TICK - 1)
                             / (uint64_t)NS_PER_TICK;
            if (timeout_ticks > 0)
                timeout_abs = timer_get_ticks() + timeout_ticks;
            else
                timeout_abs = timer_get_ticks() + 1; /* at least 1 tick */
        }
    }

    uint64_t __sig_flags;

    for (;;) {
        spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

        /* Check for any pending signal in our set */
        uint64_t pending = p->pending_signals & sig_set;
        if (pending) {
            /* Found one — pick the lowest-numbered signal */
            int signum = __builtin_ctzll(pending);
            if (signum > 0 && signum < SIG_MAX) {
                p->pending_signals &= ~(1ULL << signum);

                /* Capture siginfo if available */
                struct siginfo saved_info;
                memset(&saved_info, 0, sizeof(saved_info));
                int have_info = 0;
                if (p->sig_info[signum].si_signo == signum) {
                    memcpy(&saved_info, &p->sig_info[signum], sizeof(saved_info));
                    memset(&p->sig_info[signum], 0, sizeof(struct siginfo));
                    have_info = 1;
                } else {
                    saved_info.si_signo = signum;
                    saved_info.si_code  = SI_USER;
                }

                p->sigwait_mask = 0;
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);

                /* Copy info to userspace if requested */
                if (info_addr) {
                    if (copy_to_user(info_addr, &saved_info,
                                     sizeof(saved_info)) < 0)
                        return (uint64_t)(int64_t)-EFAULT;
                }

                return (uint64_t)(int64_t)signum;
            }
        }

        /* Poll mode: no pending signal, return immediately */
        if (poll_mode) {
            p->sigwait_mask = 0;
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return (uint64_t)(int64_t)-EAGAIN;
        }

        /* Check timeout expiration */
        if (timeout_abs != 0) {
            uint64_t now = timer_get_ticks();
            if (now >= timeout_abs) {
                p->sigwait_mask = 0;
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                return (uint64_t)(int64_t)-EAGAIN;
            }
        }

        /* Block until a signal arrives (woken by signal_send) or timeout */
        p->sigwait_mask = sig_set;
        if (timeout_abs != 0)
            p->sleep_until = timeout_abs;
        else
            p->sleep_until = 0;  /* unlimited wait */

        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);

        scheduler_yield();

        /* Woken up — loop back and re-check pending signals.
         * signal_send (with sigwait_mask match) or scheduler_wake_sleepers
         * (timeout expired) woke us.  sigwait_mask is cleared by the
         * waker. */
    }
}

/* ── sys_kill — send signal to process/process group ────────────
 *
 *   int kill(pid_t pid, int sig);
 *
 * Linux-compatible kill syscall.  Send signal `sig` to the process(es)
 * identified by `pid`:
 *   pid > 0   → send to the process with that PID
 *   pid == 0  → send to all processes in the caller's process group
 *   pid == -1 → send to all processes for which the caller has permission
 *   pid < -1  → send to the process group with ID = -pid
 *
 * If sig == 0, perform error checking only — no signal is actually sent.
 * This is still subject to the same permission checks, and is used by
 * userspace to probe whether a process/group exists and is killable.
 *
 * Returns 0 on success, -errno on error.
 */
uint64_t sys_kill(uint64_t pid, uint64_t sig) {
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    /* Basic signal number sanity — sig == 0 is the null-signal probe,
     * sig > SIG_MAX is invalid, everything 1..SIG_MAX is valid. */
    if (sig > SIG_MAX)
        return (uint64_t)(int64_t)-EINVAL;

    /* ── pid > 0: send to a specific process ──────────────────── */
    if ((int64_t)pid > 0) {
        struct process *target = process_get_by_pid((uint32_t)pid);
        if (!target || target->state == PROCESS_UNUSED)
            return (uint64_t)(int64_t)-ESRCH;

        /* Permission check — root (euid 0) or matching UID can signal */
        if (cur->euid != 0 &&
            cur->euid != target->euid &&
            cur->uid  != target->uid)
            return (uint64_t)(int64_t)-EPERM;

        /* Null-signal probe: existence+permission check only */
        if (sig == 0)
            return 0;

        return (uint64_t)(int64_t)signal_send((uint32_t)pid, (int)sig);
    }

    /* ── pid == 0: send to all processes in caller's group ──── */
    if ((int64_t)pid == 0) {
        uint32_t pgid = cur->pgid;
        struct process *table = process_get_table();
        int found = 0;

        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state == PROCESS_UNUSED)
                continue;
            if (table[i].pgid != pgid)
                continue;
            found = 1;
            if (sig != 0)
                signal_send(table[i].pid, (int)sig);
        }

        if (!found)
            return (uint64_t)(int64_t)-ESRCH;
        return 0;
    }

    /* ── pid == -1: broadcast to all processes ────────────────── */
    if ((int64_t)pid == -1) {
        struct process *table = process_get_table();

        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state == PROCESS_UNUSED)
                continue;
            /* Skip ourselves (PID 1 / init is also excluded) */
            if (table[i].pid == cur->pid || table[i].pid == 1)
                continue;
            if (sig != 0) {
                /* Permission check per target */
                if (cur->euid == 0 ||
                    cur->euid == table[i].euid ||
                    cur->uid  == table[i].uid)
                    signal_send(table[i].pid, (int)sig);
            }
        }
        /* Broadcast null-probe always succeeds (we can always signal
         * at least ourselves, but we skip PID 1, so just return 0). */
        return 0;
    }

    /* ── pid < -1: send to process group with ID = -pid ─────── */
    {
        uint32_t pgid = (uint32_t)(-(int64_t)pid);

        if (pgid == 0)
            return (uint64_t)(int64_t)-EINVAL;

        /* Null-signal probe for process group */
        if (sig == 0) {
            struct process *table = process_get_table();
            for (int i = 0; i < PROCESS_MAX; i++) {
                if (table[i].state == PROCESS_UNUSED)
                    continue;
                if (table[i].pgid == pgid)
                    return 0;
            }
            return (uint64_t)(int64_t)-ESRCH;
        }

        return (uint64_t)(int64_t)signal_send_group(pgid, (int)sig);
    }
}
