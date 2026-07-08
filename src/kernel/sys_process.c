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
#define KERNEL_INTERNAL
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
                if (p->sig_info[signum].si_signo == signum) {
                    memcpy(&saved_info, &p->sig_info[signum], sizeof(saved_info));
                    memset(&p->sig_info[signum], 0, sizeof(struct siginfo));
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

/* ── sys_tkill — send signal to a single thread ─────────────────
 *
 *   int tkill(pid_t tid, int sig);
 *
 * Linux-compatible tkill syscall.  Sends signal `sig` to the single
 * thread identified by `tid` (which is the PID of the thread).
 *
 * Unlike kill(pid > 0) which sends to the main process (thread group
 * leader), tkill targets a specific thread by its TID regardless of
 * whether it is a thread within a thread group.
 *
 * This sets si_code = SI_TKILL in the siginfo to distinguish the
 * signal source from kill(2) (which uses SI_USER).
 *
 * If sig == 0, perform error checking only — no signal is actually
 * sent.  This null-signal probe is used by userspace to verify that
 * a specific TID exists and is killable.
 *
 * Returns 0 on success, -errno on error.
 */
uint64_t sys_tkill(uint64_t pid, uint64_t sig)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    /* Basic signal number sanity — sig == 0 is the null-signal probe,
     * sig > SIG_MAX is invalid, everything 1..SIG_MAX is valid. */
    if (sig > SIG_MAX)
        return (uint64_t)(int64_t)-EINVAL;

    /* Find the target thread by PID */
    struct process *target = process_get_by_pid((uint32_t)pid);
    if (!target || target->state == PROCESS_UNUSED)
        return (uint64_t)(int64_t)-ESRCH;

    /* Permission check — root (euid 0) or matching UID can signal.
     * Follows POSIX.1-2008 semantics: either the real or effective
     * UID of the caller must match the real or saved UID of the target.
     * This kernel tracks real uid and effective uid per process. */
    if (cur->euid != 0 &&
        cur->euid != target->euid &&
        cur->uid  != target->uid)
        return (uint64_t)(int64_t)-EPERM;

    /* Null-signal probe: existence + permission check only */
    if (sig == 0)
        return 0;

    /* Build siginfo with SI_TKILL to distinguish from kill(2) */
    struct siginfo info;
    memset(&info, 0, sizeof(info));
    info.si_signo = (int)sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = cur->pid;
    info.si_uid   = cur->uid;

    /* Deliver the signal via signal_send_info which handles
     * the si_code, pending bit setting, and process wakeup.
     * Returns 0 on success, -1 on failure. */
    if (signal_send_info((uint32_t)pid, (int)sig, &info) < 0)
        return (uint64_t)(int64_t)-EAGAIN;

    return 0;
}

/* ── sys_tgkill — send signal to thread in a specific thread group ─
 *
 *   int tgkill(int tgid, int tid, int sig);
 *
 * Linux-compatible tgkill syscall.  Sends signal `sig` to the thread
 * identified by `tid` within the thread group `tgid`.  This is a
 * more robust version of tkill(2) that also verifies that `tid`
 * actually belongs to `tgid`, preventing race conditions where the
 * thread identified by `tid` might have exited and its TID been
 * reused by an unrelated process.
 *
 * Thread group semantics:
 *   - The thread group leader has tgid == pid.
 *   - Child threads (created via clone(CLONE_THREAD)) share the
 *     same tgid as the leader.
 *   - If `tid` does not belong to `tgid`, -EINVAL is returned.
 *
 * If sig == 0, perform error checking only — no signal is actually
 * sent.  This null-signal probe is used by userspace to verify that
 * a specific TID exists in the given thread group and is killable.
 *
 * Returns 0 on success, -errno on error.
 */
uint64_t sys_tgkill(uint64_t tgid, uint64_t tid, uint64_t sig)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    /* Basic signal number sanity — sig == 0 is the null-signal probe,
     * sig > SIG_MAX is invalid, everything 1..SIG_MAX is valid. */
    if (sig > SIG_MAX)
        return (uint64_t)(int64_t)-EINVAL;

    /* Find the target thread by TID */
    struct process *target = process_get_by_pid((uint32_t)tid);
    if (!target || target->state == PROCESS_UNUSED)
        return (uint64_t)(int64_t)-ESRCH;

    /* Verify that the target thread belongs to the specified thread group.
     * Both the thread group leader (tgid == pid) and threads within the
     * group (tgid == leader's pid) share the same tgid field. */
    if (target->tgid != (uint32_t)tgid)
        return (uint64_t)(int64_t)-EINVAL;

    /* Permission check — root (euid 0) or matching UID can signal.
     * Follows POSIX.1-2008 semantics: either the real or effective
     * UID of the caller must match the real or saved UID of the target. */
    if (cur->euid != 0 &&
        cur->euid != target->euid &&
        cur->uid  != target->uid)
        return (uint64_t)(int64_t)-EPERM;

    /* Null-signal probe: existence + permission check only */
    if (sig == 0)
        return 0;

    /* Build siginfo with SI_TKILL to distinguish from kill(2) */
    struct siginfo info;
    memset(&info, 0, sizeof(info));
    info.si_signo = (int)sig;
    info.si_code  = SI_TKILL;
    info.si_pid   = cur->pid;
    info.si_uid   = cur->uid;

    /* Deliver the signal via signal_send_info which handles
     * the si_code, pending bit setting, and process wakeup.
     * Returns 0 on success, -1 on failure. */
    if (signal_send_info((uint32_t)tid, (int)sig, &info) < 0)
        return (uint64_t)(int64_t)-EAGAIN;

    return 0;
}

/* ── Helper: check if a process is a child matching the given
 * wait4 pid criteria.
 *
 * Returns 1 if `child` is a living (non-UNUSED) child of `parent_pid`
 * that matches the wait4 pid pattern, 0 otherwise. */
static int wait4_child_matches(const struct process *child,
                                uint32_t parent_pid,
                                uint32_t parent_pgid,
                                int64_t req_pid)
{
    if (child->state == PROCESS_UNUSED)
        return 0;
    if (child->parent_pid != parent_pid)
        return 0;
    if (child->pid == parent_pid)
        return 0;  /* skip self */

    if (req_pid > 0)
        return child->pid == (uint32_t)req_pid;
    if (req_pid == -1)
        return 1;  /* any child */
    if (req_pid == 0)
        return child->pgid == parent_pgid;
    if (req_pid < -1) {
        uint32_t pgid = (uint32_t)(-req_pid);
        return child->pgid == pgid;
    }
    return 0;
}

/* ── sys_wait4 — wait for child process state change ────────────
 *
 *   pid_t wait4(pid_t pid, int *wstatus, int options,
 *                struct rusage *rusage);
 *
 * Linux-compatible wait4 syscall.  Waits for children matching `pid`:
 *   pid  > 0  → wait for child with specific PID
 *   pid == -1 → wait for any child
 *   pid == 0  → wait for any child in the same process group
 *   pid < -1  → wait for any child in process group |pid|
 *
 * Options:
 *   WNOHANG    (1) — return 0 immediately if no child is ready
 *   WUNTRACED  (4) — also report stopped children (currently no-op:
 *                    the kernel does not distinguish stopped children)
 *   WCONTINUED (8) — also report continued children (currently no-op)
 *
 * Returns on success:
 *   The PID of the collected child (wait status written to *wstatus).
 *   0 if WNOHANG was specified and no child was ready.
 *   -errno on error:
 *     -ECHILD — no matching (unwaited) children exist
 *     -EFAULT — wstatus or rusage pointer is invalid
 *     -EINTR  — a signal was caught (not yet implemented)
 */
uint64_t sys_wait4(uint64_t pid, uint64_t wstatus_addr,
                   uint64_t options, uint64_t rusage_addr)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    int64_t req_pid = (int64_t)pid;
    uint32_t my_pid = cur->pid;
    uint32_t my_pgid = cur->pgid;
    struct process *table = process_get_table();

    for (;;) {
        int found_any = 0;

        /* Scan for matching children */
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *child = &table[i];

            if (!wait4_child_matches(child, my_pid, my_pgid, req_pid))
                continue;

            found_any = 1;

            /* Is this child ready (zombie)? */
            if (child->state == PROCESS_ZOMBIE) {
                int wstatus;

                /* Encode exit status in Linux-compatible format.
                 * The kernel stores the raw exit code in exit_code.
                 * For now, all exits are treated as WIFEXITED with
                 * WEXITSTATUS = exit_code. */
                wstatus = __W_EXITCODE(child->exit_code & 0xff, 0);

                /* Copy wait status to userspace */
                if (wstatus_addr) {
                    if (copy_to_user(wstatus_addr, &wstatus,
                                     sizeof(wstatus)) < 0)
                        return (uint64_t)(int64_t)-EFAULT;
                }

                /* Optionally fill rusage */
                if (rusage_addr) {
                    struct rusage ru;
                    memset(&ru, 0, sizeof(ru));
                    ru.ru_utime.tv_sec  = child->utime_ticks / 100;
                    ru.ru_utime.tv_usec = (child->utime_ticks % 100) * 10000;
                    ru.ru_stime.tv_sec  = child->stime_ticks / 100;
                    ru.ru_stime.tv_usec = (child->stime_ticks % 100) * 10000;
                    ru.ru_minflt  = child->minflt;
                    ru.ru_majflt  = child->majflt;
                    ru.ru_nvcsw   = child->nvcsw;
                    ru.ru_nivcsw  = child->nivcsw;
                    ru.ru_nsignals = child->signals_received;

                    if (copy_to_user(rusage_addr, &ru, sizeof(ru)) < 0)
                        return (uint64_t)(int64_t)-EFAULT;
                }

                uint32_t child_pid = child->pid;

                /* Clean up the zombie */
                process_cleanup(child);

                return (uint64_t)(int64_t)child_pid;
            }
        }

        /* No matching children at all */
        if (!found_any)
            return (uint64_t)(int64_t)-ECHILD;

        /* WNOHANG: no child ready, return 0 */
        if (options & WNOHANG)
            return 0;

        /* Block until a child becomes zombie.
         * Find the first non-zombie matching child and wait for it
         * specifically (process_wake_waiter is PID-specific). */
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *child = &table[i];
            if (!wait4_child_matches(child, my_pid, my_pgid, req_pid))
                continue;
            if (child->state != PROCESS_ZOMBIE) {
                /* Wait for this specific child */
                cur->wait_for_pid = child->pid;
                cur->state = PROCESS_BLOCKED;
                scheduler_remove(cur);
                scheduler_yield();

                /* Woken up — re-acquire current process pointer
                 * (may have changed if another CPU woke us). */
                cur = process_get_current();
                if (!cur)
                    return (uint64_t)(int64_t)-ESRCH;
                my_pid = cur->pid;
                my_pgid = cur->pgid;
                cur->wait_for_pid = 0;

                /* Re-scan children from the top — the child we
                 * waited for may now be zombie, or another child
                 * may have become ready concurrently. */
                goto rescan;
            }
        }
        /* Fallthrough: all matching children became zombies while
         * we were selecting one to wait on — loop back and collect. */
rescan:
        ;
    }
}

/* ── Helper: check if a process is a child matching the given
 * waitid criteria (which/idtype-based selection).
 *
 *   P_PID   — match child with specific PID == (uint32_t)req_id
 *   P_PGID  — match any child in process group req_id
 *   P_ALL   — match any child (regardless of req_id)
 *
 * Returns 1 if `child` is a living (non-UNUSED) child of `parent_pid`
 * matching the criteria, 0 otherwise. */
static int waitid_child_matches(const struct process *child,
                                 uint32_t parent_pid,
                                 uint32_t parent_pgid,
                                 int which,
                                 int64_t req_id)
{
    if (child->state == PROCESS_UNUSED)
        return 0;
    if (child->parent_pid != parent_pid)
        return 0;
    if (child->pid == parent_pid)
        return 0;  /* skip self */

    switch (which) {
    case P_PID:
        return (uint32_t)req_id == 0 || child->pid == (uint32_t)req_id;
    case P_PGID:
        return child->pgid == (uint32_t)req_id;
    case P_ALL:
        return 1;  /* any child */
    default:
        return 0;
    }
}

/* ── sys_waitid — wait for child with siginfo ──────────────────
 *
 *   int waitid(idtype_t which, id_t id, siginfo_t *info,
 *              int options, struct rusage *rusage);
 *
 * Linux-compatible waitid syscall.  Waits for children matching
 * `which`/`id` criteria:
 *   P_PID   (0) — wait for child with specific PID == id
 *   P_PGID  (1) — wait for any child in process group id
 *   P_ALL   (2) — wait for any child (ignores id)
 *
 * Options:
 *   WEXITED     (4) — wait for exited children (always implied)
 *   WSTOPPED    (2) — also report stopped children (currently no-op:
 *                     the kernel does not distinguish stopped children)
 *   WCONTINUED  (8) — also report continued children (currently no-op)
 *   WNOHANG     (1) — return 0 immediately if no child is ready
 *   WNOWAIT     (0x01000000) — leave child as zombie (don't clean up)
 *
 * Returns 0 on success, -errno on error.
 * On success, siginfo_t is written to *info with:
 *   si_signo = SIGCHLD
 *   si_code  = CLD_EXITED (or CLD_KILLED/CLD_DUMPED)
 *   si_pid   = child PID
 *   si_uid   = child UID
 *   si_status = child exit code
 */
uint64_t sys_waitid(uint64_t which, uint64_t id, uint64_t info_addr,
                    uint64_t options, uint64_t rusage_addr)
{
    struct process *cur = process_get_current();
    if (!cur)
        return (uint64_t)(int64_t)-ESRCH;

    int do_not_reap = (options & WNOWAIT) ? 1 : 0;
    uint32_t my_pid = cur->pid;
    uint32_t my_pgid = cur->pgid;
    struct process *table = process_get_table();

    for (;;) {
        int found_any = 0;

        /* Scan for matching children */
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *child = &table[i];

            if (!waitid_child_matches(child, my_pid, my_pgid,
                                       (int)which, (int64_t)id))
                continue;

            found_any = 1;

            /* Is this child ready (zombie)? */
            if (child->state == PROCESS_ZOMBIE) {
                struct siginfo info;
                memset(&info, 0, sizeof(info));
                info.si_signo = SIGCHLD;
                info.si_errno = 0;
                info.si_code  = CLD_EXITED;
                info.si_pid   = child->pid;
                info.si_uid   = child->uid;
                info.si_status = child->exit_code & 0xff;
                info.si_addr  = NULL;

                /* Copy siginfo to userspace */
                if (info_addr) {
                    if (copy_to_user(info_addr, &info, sizeof(info)) < 0)
                        return (uint64_t)(int64_t)-EFAULT;
                }

                /* Optionally fill rusage */
                if (rusage_addr) {
                    struct rusage ru;
                    memset(&ru, 0, sizeof(ru));
                    ru.ru_utime.tv_sec  = child->utime_ticks / 100;
                    ru.ru_utime.tv_usec = (child->utime_ticks % 100) * 10000;
                    ru.ru_stime.tv_sec  = child->stime_ticks / 100;
                    ru.ru_stime.tv_usec = (child->stime_ticks % 100) * 10000;
                    ru.ru_minflt  = child->minflt;
                    ru.ru_majflt  = child->majflt;
                    ru.ru_nvcsw   = child->nvcsw;
                    ru.ru_nivcsw  = child->nivcsw;
                    ru.ru_nsignals = child->signals_received;

                    if (copy_to_user(rusage_addr, &ru, sizeof(ru)) < 0)
                        return (uint64_t)(int64_t)-EFAULT;
                }

                /* Clean up the zombie unless WNOWAIT */
                if (!do_not_reap)
                    process_cleanup(child);

                return 0;  /* waitid returns 0 on success, not PID */
            }
        }

        /* No matching children at all */
        if (!found_any)
            return (uint64_t)(int64_t)-ECHILD;

        /* WNOHANG: no child ready, return 0 */
        if (options & WNOHANG)
            return 0;

        /* Block until a child becomes zombie.
         * Find the first non-zombie matching child and wait for it
         * specifically (process_wake_waiter is PID-specific). */
        for (int i = 0; i < PROCESS_MAX; i++) {
            struct process *child = &table[i];
            if (!waitid_child_matches(child, my_pid, my_pgid,
                                       (int)which, (int64_t)id))
                continue;
            if (child->state != PROCESS_ZOMBIE) {
                cur->wait_for_pid = child->pid;
                cur->state = PROCESS_BLOCKED;
                scheduler_remove(cur);
                scheduler_yield();

                /* Woken up — re-acquire current process pointer
                 * (may have changed if another CPU woke us). */
                cur = process_get_current();
                if (!cur)
                    return (uint64_t)(int64_t)-ESRCH;
                my_pid = cur->pid;
                my_pgid = cur->pgid;
                cur->wait_for_pid = 0;
                goto rescan;
            }
        }
        /* Fallthrough: all matching children became zombies while
         * we were selecting one to wait on — loop back and collect. */
rescan:
        ;
    }
}

/* ── sys_exit_group — exit all threads in the current thread group ─
 *
 *   void exit_group(int status);
 *
 * Linux-compatible exit_group syscall.  Terminates all threads in
 * the calling process's thread group with the given exit status.
 *
 * Implementation: sends SIGKILL to every other thread in the group,
 * then calls process_exit_code(status) for the current thread.
 * Threads that receive SIGKILL will be torn down by the signal
 * dispatch path (signal_deliver -> process_exit_code(9)).
 *
 * This function does not return (calls process_exit_code which
 * terminates the current process).
 */
uint64_t sys_exit_group(uint64_t code)
{
	struct process *cur = process_get_current();
	if (!cur)
		return (uint64_t)(int64_t)-ESRCH;

	uint32_t my_tgid = cur->tgid;
	uint32_t my_pid = cur->pid;

	/* Iterate the process table and signal every thread in the same group,
	 * except the caller (which exits explicitly below). */
	struct process *table = process_get_table();
	for (int i = 0; i < PROCESS_MAX; i++) {
		struct process *p = &table[i];
		if (p->state == PROCESS_UNUSED)
			continue;
		if (p->tgid != my_tgid)
			continue;
		if (p->pid == my_pid)
			continue;

		struct siginfo info;
		info.si_signo = SIGKILL;
		info.si_errno = 0;
		info.si_code  = SI_KERNEL;
		info.si_pid   = my_pid;
		info.si_uid   = cur->uid;
		info.si_addr  = NULL;
		info.si_status = 0;

		signal_send_info(p->pid, SIGKILL, &info);
	}

	/* Terminate the caller — this doesn't return. */
	process_exit_code((int)code);

	/* Unreachable */
	return 0;
}
