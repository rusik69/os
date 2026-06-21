/*
 * Signal handling — real-time signals + siginfo_t delivery
 *
 * Supports signals 1-64 (SIGRTMIN=32..SIGRTMAX=64).
 * Real-time signals (SIGRTMIN-SIGRTMAX) are queued with siginfo_t.
 * SIGCHLD delivers exit status via siginfo_t.
 */
#include "signal.h"
#include "process.h"
#include "scheduler.h"
#include "printf.h"
#include "syscall.h"
#include "errno.h"
#include "signal_validate.h"

/* Core dump handler */
extern void do_coredump(struct process *proc, int signo);

int signal_send(uint32_t pid, int signum) {
    if (signum == 0) {
        struct process *probe = process_get_by_pid(pid);
        return (probe && probe->state != PROCESS_UNUSED) ? 0 : -1;
    }
    if (signum <= 0 || signum >= SIG_MAX) return -1;
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    /* Acquire sig_lock to protect pending_signals/state/exit_code */
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    /* RLIMIT_SIGPENDING: enforce pending signal queue limit.
     * Exclude SIGKILL (always deliverable) and SIGSTOP (cannot be blocked). */
    if (signum != SIGKILL && signum != SIGSTOP) {
        uint64_t sig_limit = p->rlim_cur[RLIMIT_SIGPENDING];
        if (sig_limit != ~0ULL) {
            /* Count currently pending (non-masked) signals */
            uint64_t pending = p->pending_signals;
            /* Also count masked signals (they just hang out) */
            int count = 0;
            while (pending) { pending &= pending - 1; count++; }
            /* If at or over limit, reject the signal */
            if ((uint64_t)count >= sig_limit) {
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                return -EAGAIN;
            }
        }
    }

    /* Track resource usage */
    p->signals_received++;

    /* Permission check */
    struct process *caller = process_get_current();
    if (caller && caller->pid != p->pid) {
        if (!process_can_see(caller, p)) {
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return -1;
        }
    }

    if (p->state == PROCESS_ZOMBIE) {
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    /* SIGKILL — immediate terminate */
    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* SIGCONT — always resumes, regardless of mask */
    if (signum == SIGCONT) {
        p->pending_signals |= (1ULL << signum);
        if (p->is_suspended) {
            p->is_suspended = 0;
            p->sleep_until = 0;
            p->state = PROCESS_READY;
            scheduler_add(p);
        }
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    /* If masked, just set pending */
    if (p->sig_mask & (1ULL << signum)) {
        p->pending_signals |= (1ULL << signum);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        return 0;
    }

    if (signum == SIGTERM) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    if (signum == SIGSTOP || signum == SIGTSTP || signum == SIGTTIN || signum == SIGTTOU) {
        p->is_suspended = 1;
        p->sleep_until = 0;
        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    p->pending_signals |= (1ULL << signum);

    /* Notify signalfd listeners */
    extern void signalfd_notify(int signum);
    signalfd_notify(signum);

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
    return 0;
}

/* Extended signal send with siginfo_t support.
 * Stores siginfo for delivery by signal_check (or signal fd read).
 * For real-time signals (SIGRTMIN-SIGRTMAX) the info is queued.
 * For standard signals, keeps the most recent info only.
 * Validates siginfo fields before storing (security).
 *
 * Note: This function acquires the process's sig_lock internally
 * so it is SMP-safe.  It does NOT call signal_send() separately
 * to avoid a TOCTOU race between setting the pending bit and
 * storing the siginfo.
 */
int signal_send_info(uint32_t pid, int signum, struct siginfo *info) {
    if (signum <= 0 || signum >= SIG_MAX) return -1;

    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED || p->state == PROCESS_ZOMBIE)
        return -1;

    /* Hold the lock for the entire operation */
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    /* Permission check */
    struct process *caller = process_get_current();
    if (caller && caller->pid != p->pid) {
        if (!process_can_see(caller, p)) {
            spinlock_irqsave_release(&p->sig_lock, __sig_flags);
            return -1;
        }
    }

    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        spinlock_irqsave_release(&p->sig_lock, __sig_flags);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* Set the pending signal bit */
    p->pending_signals |= (1ULL << signum);

    /* Store siginfo if provided */
    if (info) {
        struct siginfo validated = *info;
        int is_from_userspace = (caller && caller->is_user) ? 1 : 0;
        signal_validate_siginfo(&validated, is_from_userspace);
        p->sig_info[signum] = validated;
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);

    /* Notify signalfd listeners */
    extern void signalfd_notify(int signum);
    signalfd_notify(signum);
    if (info) {
        extern void signalfd_notify_ext(int signum, int si_code,
                                        uint32_t si_pid, uint32_t si_uid,
                                        uint64_t si_addr, int si_status);
        signalfd_notify_ext(signum, info->si_code, info->si_pid, info->si_uid,
                           (uint64_t)(uintptr_t)info->si_addr, info->si_status);
    }

    return 0;
}

int signal_send_group(uint32_t pgid, int signum) {
    if (pgid == 0) return -1;
    struct process *table = process_get_table();
    int sent = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED || table[i].pgid != pgid) continue;
        if (signal_send(table[i].pid, signum) == 0) sent++;
    }
    return sent > 0 ? 0 : -1;
}

int signal_send_pgid(uint32_t pgid, int signum) {
    return signal_send_group(pgid, signum);
}

/* Retrieve the siginfo for a signal, if available.
 * Returns the siginfo pointer, or NULL if no info stored.
 * Clears the stored info after returning it (one-shot). */
struct siginfo *signal_get_info(struct process *p, int signum) {
    if (!p || signum <= 0 || signum >= SIG_MAX) return NULL;
    if (!(p->pending_signals & (1ULL << signum))) return NULL;
    /* Only return info if it was explicitly set (non-zero si_signo) */
    if (p->sig_info[signum].si_signo == signum) {
        return &p->sig_info[signum];
    }
    return NULL;
}

void signal_check(void) {
    struct process *p = process_get_current();
    if (!p || !p->pending_signals) return;

    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(p->pending_signals & (1ULL << sig))) continue;

        /* Skip masked signals */
        if (p->sig_mask & (1ULL << sig)) continue;

        p->pending_signals &= ~(1ULL << sig);

        signal_handler_t handler = p->sig_handlers[sig];

        if (handler == SIG_IGN) {
            continue;
        }

        if (handler != SIG_DFL) {
            /* User handler installed — release lock while handler runs */
            if (!p->is_user) {
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                handler(sig);
                spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
            }
            continue;
        }

        /* Default actions */
        switch (sig) {
            case SIGSEGV:
            case SIGQUIT:
            case SIGABRT:
                do_coredump(p, sig);
                /* fall through */
            case SIGKILL:
            case SIGTERM:
            case SIGPIPE:
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                return; /* zombie — never resumes */
            case SIGCHLD:
                /* Default for SIGCHLD is to ignore */
                break;
            case SIGSTOP:
            case SIGTSTP:
            case SIGTTIN:
            case SIGTTOU:
                p->is_suspended = 1;
                p->state = PROCESS_BLOCKED;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                /* Resumed by SIGCONT — re-acquire and check more signals */
                spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
                continue;
            case SIGCONT:
                break;
            default:
                /* For real-time signals (SIGRTMIN+) with no handler: ignore */
                if (sig >= SIGRTMIN && sig <= SIGRTMAX)
                    break;
                /* Unknown signal with default: terminate */
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                spinlock_irqsave_release(&p->sig_lock, __sig_flags);
                scheduler_yield();
                return; /* zombie — never resumes */
        }
    }

    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_register(int signum, signal_handler_t handler) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_handlers[signum] = handler;
    p->sig_flags[signum] = 0;  /* No SA flags when using signal() */
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_register_flags(int signum, signal_handler_t handler, uint32_t flags) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_handlers[signum] = handler;
    p->sig_flags[signum] = flags;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_mask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_mask |= sigmask;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

void signal_unmask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    uint64_t __sig_flags;
    spinlock_irqsave_acquire(&p->sig_lock, &__sig_flags);
    p->sig_mask &= ~sigmask;
    spinlock_irqsave_release(&p->sig_lock, __sig_flags);
}

/* ── Stub: signal_handle ─────────────────────────────── */
int signal_handle(void *task, int sig)
{
    (void)task;
    (void)sig;
    kprintf("[signal] signal_handle: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: signal_register_handler ─────────────────────────────── */
int signal_register_handler(int sig, void *handler)
{
    (void)sig;
    (void)handler;
    kprintf("[signal] signal_register_handler: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: signal_block ─────────────────────────────── */
int signal_block(int sig)
{
    (void)sig;
    kprintf("[signal] signal_block: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: signal_unblock ─────────────────────────────── */
int signal_unblock(int sig)
{
    (void)sig;
    kprintf("[signal] signal_unblock: not yet implemented\n");
    return -ENOSYS;
}
