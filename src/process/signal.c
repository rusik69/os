#include "signal.h"
#include "process.h"
#include "scheduler.h"

int signal_send(uint32_t pid, int signum) {
    if (signum == 0) {
        struct process *probe = process_get_by_pid(pid);
        return (probe && probe->state != PROCESS_UNUSED) ? 0 : -1;
    }
    if (signum <= 0 || signum >= SIG_MAX) return -1;
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    /* SIGKILL cannot be masked or ignored — immediate terminate */
    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* If signal is masked, just set the pending bit — deliver when unmasked */
    if (p->sig_mask & (1u << signum)) {
        p->pending_signals |= (1u << signum);
        return 0;
    }

    if (signum == SIGTERM) {
        p->state = PROCESS_ZOMBIE;
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

    p->pending_signals |= (1u << signum);

    if (signum == SIGCONT && p->is_suspended) {
        p->is_suspended = 0;
        p->sleep_until = 0;
        p->state = PROCESS_READY;
        scheduler_add(p);
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

void signal_check(void) {
    struct process *p = process_get_current();
    if (!p || !p->pending_signals) return;

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(p->pending_signals & (1u << sig))) continue;

        /* Skip masked signals — leave them pending until unmasked */
        if (p->sig_mask & (1u << sig)) continue;

        p->pending_signals &= ~(1u << sig);

        signal_handler_t handler = p->sig_handlers[sig];

        if (handler == SIG_IGN) {
            /* Ignored — do nothing */
            continue;
        }

        if (handler != SIG_DFL) {
            /* User handler installed — call it */
            handler(sig);
            continue;
        }

        /* Default actions */
        switch (sig) {
            case SIGKILL:
            case SIGTERM:
            case SIGPIPE:
                p->state = PROCESS_ZOMBIE;
                scheduler_remove(p);
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
                scheduler_yield();
                break;
            case SIGCONT:
                /* Already handled in signal_send */
                break;
            default:
                /* Unknown signal with default action: terminate */
                p->state = PROCESS_ZOMBIE;
                scheduler_remove(p);
                scheduler_yield();
                break;
        }
    }
}

void signal_register(int signum, signal_handler_t handler) {
    if (signum <= 0 || signum >= SIG_MAX) return;
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_handlers[signum] = handler;
}

void signal_mask(uint32_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_mask |= sigmask;
}

void signal_unmask(uint32_t sigmask) {
    struct process *p = process_get_current();
    if (!p) return;
    p->sig_mask &= ~sigmask;
}
