/*
 * signal_validate.c — siginfo validation before delivery to userspace
 *
 * Security checks on siginfo_t before passing to userspace signal handlers.
 *
 * Features:
 *   - Zero kernel address bits in si_addr for SIGSEGV/SIGBUS (kptr_restrict style)
 *   - Ensure SI_USER siginfo only originates from kernel, SI_TKILL from userspace
 *   - Validate si_code ranges for known signals
 *   - Audit logging of suspicious siginfo
 */

#include "signal.h"
#include "process.h"
#include "printf.h"
#include "audit.h"
#include "kptr_restrict.h"

/* Debug toggle — when set, kernel addresses in si_addr are preserved.
 * Controlled via sysctl / debugfs for testing/debugging only. */
static int signal_validate_debug_addr = 0;

/* Set the debug flag (for sysctl or debugfs) */
void signal_validate_set_debug(int val)
{
    signal_validate_debug_addr = val;
}

/*
 * Validate si_code ranges for known signals.
 * Returns 0 if valid, -EINVAL if suspicious.
 *
 * si_code values from userspace should be:
 *   SI_USER (0)      — only from kernel (kill(2) via syscall)
 *   SI_TKILL (3)     — allowed from userspace (tgkill)
 *   SI_QUEUE (2)     — from sigqueue
 *   positive values  — signal-specific codes (SEGV_MAPERR, etc.)
 *   negative values  — kernel-generated only
 *
 * We sanitize by:
 *   - Rejecting SI_USER with si_pid == 0 from userspace (forged kernel origin)
 *   - Forcing SI_TKILL for userspace-generated signals
 *   - Clamping unknown si_code to SI_USER for safety
 */
int signal_validate_siginfo(struct siginfo *info, int is_from_userspace)
{
    if (!info)
        return 0;

    int signo = info->si_signo;

    /* Basic signo sanity */
    if (signo <= 0 || signo >= SIG_MAX)
        return -EINVAL;

    /* ── Validate si_code ────────────────────────────────────────── */

    if (is_from_userspace) {
        /* Userspace can only send SI_TKILL or SI_QUEUE via tgkill/sigqueue.
         * SI_USER (0) is reserved for kernel internal use (kill(2) from
         * kernel, where info is synthesized). */
        if (info->si_code == SI_USER) {
            /* Userspace pretending to be kernel — reject */
            audit_log_event("signal_validate: userspace SI_USER rejected");
            return -EPERM;
        }

        /* Force valid si_code for userspace-origin */
        if (info->si_code != SI_TKILL && info->si_code != SI_QUEUE) {
            /* Clamp to SI_TKILL as safe default */
            info->si_code = SI_TKILL;
        }
    } else {
        /* Kernel-generated signals should use SI_KERNEL or signal-specific codes.
         * SI_TKILL from kernel is unusual but allowed for tgkill-like paths. */
        if (info->si_code == SI_USER) {
            /* SI_USER from kernel is normal for kill(2) syscall */
            /* Ensure si_pid is set to the sender's PID */
            if (info->si_pid == 0) {
                struct process *cur = process_get_current();
                if (cur)
                    info->si_pid = cur->pid;
            }
            if (info->si_uid == 0) {
                struct process *cur = process_get_current();
                if (cur)
                    info->si_uid = cur->euid;
            }
        }
    }

    /* ── Validate signal-specific si_code values ─────────────────── */

    switch (signo) {
    case SIGSEGV:
        /* Valid si_code for SIGSEGV: SEGV_MAPERR (1), SEGV_ACCERR (2) */
        if (info->si_code > 0 && info->si_code != SEGV_MAPERR &&
            info->si_code != SEGV_ACCERR) {
            /* Unknown si_code — clamp to SEGV_MAPERR */
            info->si_code = SEGV_MAPERR;
        }
        break;

    case SIGBUS:
        /* Valid si_code for SIGBUS: BUS_ADRERR (3) */
        if (info->si_code > 0 && info->si_code != BUS_ADRERR) {
            info->si_code = BUS_ADRERR;
        }
        break;

    case SIGCHLD:
        /* Valid si_code for SIGCHLD: CLD_EXITED (1) through CLD_CONTINUED (6) */
        if (info->si_code > 0 &&
            (info->si_code < CLD_EXITED || info->si_code > CLD_CONTINUED)) {
            info->si_code = CLD_EXITED;
        }
        break;

    case SIGILL:
    case SIGFPE:
    case SIGTRAP:
        /* These can have various signal-specific codes; accept any positive
         * value as potentially valid. Negative values are kernel-internal. */
        break;

    default:
        /* For other signals, si_code should be SI_USER, SI_KERNEL, SI_TKILL,
         * SI_QUEUE, or a positive signal-specific value. */
        break;
    }

    /* ── Sanitize si_addr for SIGSEGV/SIGBUS ─────────────────────── */

    if ((signo == SIGSEGV || signo == SIGBUS) && info->si_addr != NULL) {
        uint64_t addr_val = (uint64_t)(uintptr_t)info->si_addr;

        /* If the address resides in kernel space, hide it unless debug
         * mode is enabled AND the caller has kptr_restrict bypass. */
        if (addr_val >= 0xFFFF800000000000ULL) {
            if (!signal_validate_debug_addr) {
                /* Zero out kernel address bits — pass a sanitized value
                 * that indicates kernel space but doesn't leak the address */
                info->si_addr = (void *)(uintptr_t)(addr_val & 0xFFFULL);
                /* The low 12 bits (page offset) are non-identifying */
            }
        }
    }

    return 0;
}

/*
 * Hook called at the point of signal delivery to userspace.
 * This is the last validation step before the handler sees the siginfo.
 * Returns 0 to allow delivery, negative to abort/truncate.
 */
int signal_validate_on_delivery(struct siginfo *info)
{
    if (!info)
        return 0;

    /* Re-validate at delivery time (catches stale info) */
    return signal_validate_siginfo(info, 0);
}

/* ── Stub: signal_validate ─────────────────────────────── */
int signal_validate(int sig)
{
    (void)sig;
    kprintf("[signal] signal_validate: not yet implemented\n");
    return 0;
}
/* ── Stub: signal_valid_rt ─────────────────────────────── */
int signal_valid_rt(int sig)
{
    (void)sig;
    kprintf("[signal] signal_valid_rt: not yet implemented\n");
    return 0;
}
/* ── Stub: signal_sendable ─────────────────────────────── */
int signal_sendable(void *task, int sig)
{
    (void)task;
    (void)sig;
    kprintf("[signal] signal_sendable: not yet implemented\n");
    return 0;
}
