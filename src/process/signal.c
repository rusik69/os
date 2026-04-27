#include "signal.h"
#include "process.h"
#include "scheduler.h"

int signal_send(uint32_t pid, int signum) {
    if (signum <= 0 || signum >= SIG_MAX) return -1;
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    p->pending_signals |= (1u << signum);

    /* SIGCONT: wake blocked processes */
    if (signum == SIGCONT && p->state == PROCESS_BLOCKED) {
        p->state = PROCESS_READY;
        scheduler_add(p);
    }
    return 0;
}

void signal_check(void) {
    struct process *p = process_get_current();
    if (!p || !p->pending_signals) return;

    for (int sig = 1; sig < SIG_MAX; sig++) {
        if (!(p->pending_signals & (1u << sig))) continue;

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
                p->state = PROCESS_ZOMBIE;
                scheduler_remove(p);
                scheduler_yield();
                break;
            case SIGSTOP:
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
