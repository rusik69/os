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

    /* Do not deliver signals to zombie processes */
    if (p->state == PROCESS_ZOMBIE) return 0;

    /* SIGKILL cannot be masked or ignored — immediate terminate */
    if (signum == SIGKILL) {
        p->state = PROCESS_ZOMBIE;
        p->exit_code = 128 + signum;
        p->is_suspended = 0;
        p->sleep_until = 0;
        scheduler_remove(p);
        if (p == process_get_current()) scheduler_yield();
        return 0;
    }

    /* SIGCONT always resumes a stopped process, regardless of mask (POSIX) */
    if (signum == SIGCONT) {
        p->pending_signals |= (1u << signum);
        if (p->is_suspended) {
            p->is_suspended = 0;
            p->sleep_until = 0;
            p->state = PROCESS_READY;
            scheduler_add(p);
        }
        return 0;
    }

    /* If signal is masked, just set the pending bit — deliver when unmasked */
    if (p->sig_mask & (1u << signum)) {
        p->pending_signals |= (1u << signum);
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

    p->pending_signals |= (1u << signum);

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

    /* Disable interrupts while manipulating the pending-signals bitmap
     * to prevent a timer interrupt from re-entering signal_check for
     * the same process mid-way through bit manipulation. */
    __asm__ volatile("cli");

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
            /* User handler installed — only call if we are in kernel mode.
             * User processes cannot safely register custom handlers because
             * we lack a proper user-mode signal delivery mechanism; calling
             * a user-provided address from ring-0 is a privilege escalation. */
            if (!p->is_user) {
                __asm__ volatile("sti");
                handler(sig);
                __asm__ volatile("cli");
            }
            continue;
        }

        /* Default actions */
        switch (sig) {
            case SIGKILL:
            case SIGTERM:
            case SIGPIPE:
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                __asm__ volatile("sti");
                scheduler_yield();
                /* not reached */
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
                /* not reached */
                break;
            case SIGCONT:
                /* Already handled in signal_send */
                break;
            default:
                /* Unknown signal with default action: terminate */
                p->state = PROCESS_ZOMBIE;
                p->exit_code = 128 + sig;
                scheduler_remove(p);
                __asm__ volatile("sti");
                scheduler_yield();
                /* not reached */
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
