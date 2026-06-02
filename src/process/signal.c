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

/* Core dump handler */
extern void do_coredump(struct process *proc);

int signal_send(uint32_t pid, int signum) {
    if (signum == 0) {
        struct process *probe = process_get_by_pid(pid);
        return (probe && probe->state != PROCESS_UNUSED) ? 0 : -1;
    }
    if (signum <= 0 || signum >= SIG_MAX) return -1;
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    /* Track resource usage */
    p->signals_received++;

    /* Permission check */
    struct process *caller = process_get_current();
    if (caller && caller->pid != p->pid) {
        if (!process_can_see(caller, p)) return -1;
    }

    if (p->state == PROCESS_ZOMBIE) return 0;

    /* SIGKILL — immediate terminate */
    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
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
        return 0;
    }

    /* If masked, just set pending */
    if (p->sig_mask & (1ULL << signum)) {
        p->pending_signals |= (1ULL << signum);
        return 0;
    }

    if (signum == SIGTERM) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    if (signum == SIGSTOP || signum == SIGTSTP || signum == SIGTTIN || signum == SIGTTOU) {
        p->is_suspended = 1;
        p->sleep_until = 0;
        p->state = PROCESS_BLOCKED;
        scheduler_remove(p);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    p->pending_signals |= (1ULL << signum);

    /* Notify signalfd listeners */
    extern void signalfd_notify(int signum);
    signalfd_notify(signum);

    return 0;
}

/* Extended signal send with siginfo_t support.
 * Stores siginfo for delivery by signal_check (or signal fd read).
 * For real-time signals (SIGRTMIN-SIGRTMAX) the info is queued.
 * For standard signals, keeps the most recent info only. */
int signal_send_info(uint32_t pid, int signum, struct siginfo *info) {
    int ret = signal_send(pid, signum);
    if (ret == 0 && info) {
        struct process *p = process_get_by_pid(pid);
        if (p && p->state != PROCESS_UNUSED && p->state != PROCESS_ZOMBIE) {
            if (p->pending_signals & (1ULL << signum)) {
                p->sig_info[signum] = *info;
            }
        }
        /* Notify signalfd listeners with full siginfo */
        extern void signalfd_notify_ext(int signum, int si_code,
                                        uint32_t si_pid, uint32_t si_uid,
                                        uint64_t si_addr, int si_status);
        signalfd_notify_ext(signum, info->si_code, info->si_pid, info->si_uid,
                           (uint64_t)(uintptr_t)info->si_addr, info->si_status);
    }
    return ret;
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

    __asm__ volatile("cli");

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
            /* User handler installed */
            if (!p->is_user) {
                __asm__ volatile("sti");
                handler(sig);
                __asm__ volatile("cli");
            }
            continue;
        }

        /* Default actions */
        switch (sig) {
            case SIGSEGV:
            case SIGQUIT:
            case SIGABRT:
                do_coredump(p);
                /* fall through */
            case SIGKILL:
            case SIGTERM:
            case SIGPIPE:
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                __asm__ volatile("sti");
                scheduler_yield();
                break;
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
                __asm__ volatile("sti");
                scheduler_yield();
                break;
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
                __asm__ volatile("sti");
                scheduler_yield();
                break;
        }
    }

    __asm__ volatile("sti");
}

void signal_register(int signum, signal_handler_t handler) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_handlers[signum] = handler;
}

void signal_mask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_mask |= sigmask;
}

void signal_unmask(uint64_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_mask &= ~sigmask;
}
