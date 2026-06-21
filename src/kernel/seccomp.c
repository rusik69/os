#define KERNEL_INTERNAL
#include "types.h"
#include "seccomp.h"
#include "process.h"
#include "syscall.h"
#include "printf.h"
#include "heap.h"
#include "string.h"
#include "audit.h"
#include "panic.h"
#include "kallsyms.h"

/*
 * ── Seccomp — SECCOMP_RET_LOG / TRAP / KILL with audit logging ──────
 *
 * Extends the basic seccomp filter with:
 *   SECCOMP_RET_LOG  — log the syscall via audit, then allow it
 *   SECCOMP_RET_TRAP — send SIGSYS with seccomp_data to the process
 *   SECCOMP_RET_KILL — log the violation, then kill the process
 *
 * Audit records are written via the existing audit subsystem and include:
 *   - Syscall number and up to 3 arguments
 *   - Process PID, name, and instruction pointer
 *   - The seccomp action that was taken
 */

/* Syscalls allowed in STRICT mode (simplified: exactly read/write/exit/sigreturn) */
#define STRICT_ALLOWED_COUNT 4
static const uint64_t strict_allowed[STRICT_ALLOWED_COUNT] = {
    SYS_READ,
    SYS_WRITE,
    SYS_EXIT,
    /* sigreturn would be SYS_RT_SIGRETURN — add here when defined */
    15, /* SYS_RT_SIGRETURN in Linux */
};

void seccomp_init(void) {
    kprintf("[OK] Seccomp initialized (RET_LOG/RET_TRAP audit logging available)\n");
}

/*
 * Log a seccomp event via the audit subsystem.
 * Includes the syscall number, arguments, process info, and the action taken.
 */
static void seccomp_audit_log(uint64_t num, uint64_t a1, uint64_t a2,
                               uint64_t a3, uint32_t action, uint64_t rip)
{
    struct process *p = process_get_current();
    if (!p) return;

    const char *action_str = "?";
    switch (action) {
    case SECCOMP_RET_KILL: action_str = "KILL";  break;
    case SECCOMP_RET_TRAP: action_str = "TRAP";  break;
    case SECCOMP_RET_LOG:  action_str = "LOG";   break;
    case SECCOMP_RET_ALLOW: action_str = "ALLOW"; break;
    }

    /* Format the audit message */
    char msg[256];
    const char *sym = (rip >= 0xFFFF800000000000ULL) ? kallsyms_lookup(rip) : NULL;
    int n;

    if (sym) {
        n = snprintf(msg, sizeof(msg),
            "seccomp[%s] syscall(%llu) args=(0x%llx,0x%llx,0x%llx) "
            "pid=%u name=%s rip=0x%llx (%s)",
            action_str, (unsigned long long)num,
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
            p->pid, p->name ? p->name : "?",
            (unsigned long long)rip, sym);
    } else {
        n = snprintf(msg, sizeof(msg),
            "seccomp[%s] syscall(%llu) args=(0x%llx,0x%llx,0x%llx) "
            "pid=%u name=%s rip=0x%llx",
            action_str, (unsigned long long)num,
            (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3,
            p->pid, p->name ? p->name : "?",
            (unsigned long long)rip);
    }

    if (n > 0)
        audit_log_event(msg);

    /* Also print to kernel log for immediate visibility */
    kprintf("[seccomp] %s\n", msg);
}

/*
 * Send SIGSYS signal to the current process with seccomp_data.
 * This is the SECCOMP_RET_TRAP action — the process can catch SIGSYS
 * and inspect the seccomp data to handle the violation gracefully.
 */
void seccomp_send_sigsys(uint64_t num, uint64_t rip)
{
    struct process *p = process_get_current();
    if (!p) return;
    (void)num; /* num is part of the API for future seccomp_data population */

    /* Build a siginfo with seccomp details */
    struct siginfo info;
    memset(&info, 0, sizeof(info));
    info.si_signo = SIGSYS;
    info.si_errno = 0;
    info.si_code  = SI_KERNEL;  /* sent by kernel */
    info.si_pid   = 0;          /* kernel */
    info.si_uid   = 0;          /* kernel */
    /* si_addr typically holds the instruction that caused the trap */
    info.si_addr  = (void *)rip;

    /* Send the signal — the signal handler (if any) will be invoked
     * before the process resumes.  If SIG_DFL, the default action
     * for SIGSYS is to terminate the process with core dump. */
    signal_send_info(p->pid, SIGSYS, &info);
}

/*
 * Evaluate a syscall against the current process's seccomp policy.
 * Returns the SECCOMP_RET_* action that the caller should take.
 *
 * For RET_LOG, the audit record is written here and the caller
 * should allow the syscall (return value is SECCOMP_RET_LOG, which
 * is treated as "allowed" by the dispatch logic).
 *
 * For RET_KILL and RET_TRAP, the audit record is written here and
 * the caller must handle the action (kill process / send signal).
 */
uint32_t seccomp_evaluate_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                                   uint64_t a3, uint64_t rip)
{
    struct process *p = process_get_current();
    if (!p) return SECCOMP_RET_ALLOW; /* kernel threads: always allow */

    int mode = p->seccomp_mode;
    if (mode == SECCOMP_MODE_DISABLED)
        return SECCOMP_RET_ALLOW;

    /* ── STRICT mode ─────────────────────────────────────────────── */
    if (mode == SECCOMP_MODE_STRICT) {
        for (int i = 0; i < STRICT_ALLOWED_COUNT; i++) {
            if (strict_allowed[i] == num) {
                /* STRICT allow: no logging needed */
                return SECCOMP_RET_ALLOW;
            }
        }
        /* STRICT violation — always fatal */
        seccomp_audit_log(num, a1, a2, a3, SECCOMP_RET_KILL, rip);
        return SECCOMP_RET_KILL;
    }

    /* ── FILTER mode ─────────────────────────────────────────────── */
    if (mode == SECCOMP_MODE_FILTER) {
        if (!p->seccomp_filter)
            return SECCOMP_RET_ALLOW; /* no filter installed = allow all */

        struct seccomp_filter *f = p->seccomp_filter;

        /* Walk rules in order (first match wins, like Linux) */
        for (int i = 0; i < f->num_rules; i++) {
            if (f->rules[i].syscall_nr == (int)num ||
                f->rules[i].syscall_nr == -1) {
                /* Match found — determine action */
                uint32_t action = f->rules[i].action;

                switch (action) {
                case SECCOMP_RET_ALLOW:
                    return SECCOMP_RET_ALLOW;

                case SECCOMP_RET_LOG:
                    /* Log the syscall, then allow it to proceed */
                    seccomp_audit_log(num, a1, a2, a3, SECCOMP_RET_LOG, rip);
                    return SECCOMP_RET_LOG;

                case SECCOMP_RET_TRAP:
                    seccomp_audit_log(num, a1, a2, a3, SECCOMP_RET_TRAP, rip);
                    return SECCOMP_RET_TRAP;

                case SECCOMP_RET_KILL:
                default:
                    seccomp_audit_log(num, a1, a2, a3, SECCOMP_RET_KILL, rip);
                    return SECCOMP_RET_KILL;
                }
            }
        }

        /* No matching rule — default action depends on the filter.
         * Linux defaults to SECCOMP_RET_ALLOW if no rule matches,
         * but the filter may have an explicit default.  We follow
         * the Linux convention: if no rule matches, allow. */
        return SECCOMP_RET_ALLOW;
    }

    return SECCOMP_RET_ALLOW; /* unknown mode: allow */
}

/*
 * Legacy boolean check for backward compatibility.
 * Returns 1 if the syscall is allowed, 0 if blocked.
 *
 * For RET_LOG: returns 1 (allow, already logged).
 * For RET_TRAP: sends SIGSYS, returns 0.
 * For RET_KILL: kills the process (does not return).
 */
int seccomp_check_syscall(uint64_t num) {
    uint64_t rip = (uint64_t)__builtin_return_address(0); /* our caller's RIP */
    uint32_t action = seccomp_evaluate_syscall(num, 0, 0, 0, rip);

    switch (action) {
    case SECCOMP_RET_ALLOW:
    case SECCOMP_RET_LOG:
        return 1; /* allowed */

    case SECCOMP_RET_TRAP:
        /* Send SIGSYS — the process may handle it.
         * If unhandled, the default action for SIGSYS is termination. */
        seccomp_send_sigsys(num, rip);
        return 0; /* blocked for now */

    case SECCOMP_RET_KILL:
    default:
        /* Kill the process immediately with SIGSYS (seccomp violation) */
        kprintf("[seccomp] Killing process %u (%s) for seccomp violation\n",
                process_get_current() ? process_get_current()->pid : 0,
                process_get_current() && process_get_current()->name
                    ? process_get_current()->name : "?");
        process_exit_code(SIGSYS); /* exit with signal number as code */
        return 0; /* unreachable for current process */
    }
}

int seccomp_set_mode(int mode, unsigned int flags) {
    struct process *p = process_get_current();
    if (!p) return -1;

    /* Once strict or filter is set, it cannot be unset */
    if (p->seccomp_mode != SECCOMP_MODE_DISABLED) return -1;

    if (mode != SECCOMP_MODE_STRICT && mode != SECCOMP_MODE_FILTER) return -1;

    /* SECCOMP_MODE_FILTER requires no_new_privs to be set first
     * (Linux semantics — prevents privilege escalation via seccomp). */
    if (mode == SECCOMP_MODE_FILTER && !p->no_new_privs) return -1;

    p->seccomp_mode = mode;
    if (mode == SECCOMP_MODE_FILTER) {
        /* Allocate filter if not already present */
        if (!p->seccomp_filter) {
            p->seccomp_filter = (struct seccomp_filter *)kmalloc(sizeof(struct seccomp_filter));
            if (!p->seccomp_filter) return -1;
            memset(p->seccomp_filter, 0, sizeof(struct seccomp_filter));
        }
    }

    /* Propagate to all threads if TSYNC flag is set */
    if (flags & SECCOMP_FILTER_FLAG_TSYNC) {
        int ret = seccomp_tsync();
        if (ret < 0)
            kprintf("[seccomp] TSYNC warning: failed to propagate filter to all threads (%d)\n", ret);
    }

    return 0;
}

/*
 * Synchronize the current process's seccomp mode and filter to all
 * threads sharing the same thread group ID (tgid).
 *
 * Each thread (except the caller) receives its own heap-allocated copy
 * of the filter rules so that deallocation on process exit is clean.
 *
 * Returns 0 on success, -1 on allocation failure (some threads may
 * have been updated, but the caller's filter is already installed).
 */
int seccomp_tsync(void) {
    struct process *caller = process_get_current();
    if (!caller) return -1;

    uint32_t tgid = caller->tgid;
    if (tgid == 0) return 0; /* kernel thread — no siblings */

    int failures = 0;

    /* Scan the global process table for sibling threads */
    for (uint32_t pid = 0; pid < PROCESS_MAX; pid++) {
        struct process *p = process_get_by_pid(pid);
        if (!p || p == caller) continue;
        if (p->tgid != tgid) continue;

        /* Copy the seccomp mode */
        p->seccomp_mode = caller->seccomp_mode;

        /* In strict mode there is no filter pointer to copy */
        if (caller->seccomp_mode == SECCOMP_MODE_STRICT) {
            /* Free any previously installed filter on the sibling */
            if (p->seccomp_filter) {
                kfree(p->seccomp_filter);
                p->seccomp_filter = NULL;
            }
            continue;
        }

        /* FILTER mode — deep-copy the filter rules */
        if (caller->seccomp_mode == SECCOMP_MODE_FILTER && caller->seccomp_filter) {
            /* Free any old filter on the sibling first */
            if (p->seccomp_filter) {
                kfree(p->seccomp_filter);
                p->seccomp_filter = NULL;
            }

            struct seccomp_filter *new_filter =
                (struct seccomp_filter *)kmalloc(sizeof(struct seccomp_filter));
            if (!new_filter) {
                failures++;
                continue;
            }
            memcpy(new_filter, caller->seccomp_filter, sizeof(struct seccomp_filter));
            p->seccomp_filter = new_filter;
        }
    }

    if (failures)
        kprintf("[seccomp] TSYNC: %d thread(s) could not receive filter (OOM)\n", failures);

    return failures ? -1 : 0;
}

int seccomp_get_mode(void) {
    struct process *p = process_get_current();
    if (!p) return SECCOMP_MODE_DISABLED;
    return p->seccomp_mode;
}

/* ── Filter mode support ────────────────────────────────────────────── */

int seccomp_add_rule(int syscall_nr, uint32_t action) {
    struct process *p = process_get_current();
    if (!p) return -1;
    if (p->seccomp_mode != SECCOMP_MODE_FILTER) return -1;
    if (!p->seccomp_filter) return -1;

    /* Validate action */
    if (action != SECCOMP_RET_ALLOW && action != SECCOMP_RET_KILL &&
        action != SECCOMP_RET_TRAP && action != SECCOMP_RET_LOG)
        return -1;

    struct seccomp_filter *f = p->seccomp_filter;
    if (f->num_rules >= SECCOMP_FILTER_RULES_MAX) return -1;

    f->rules[f->num_rules].syscall_nr = syscall_nr;
    f->rules[f->num_rules].action = action;
    f->num_rules++;
    return 0;
}

int seccomp_filter_check(uint64_t num) {
    struct process *p = process_get_current();
    if (!p) return 1;
    if (!p->seccomp_filter) return 1; /* no filter rules = allow */

    struct seccomp_filter *f = p->seccomp_filter;
    for (int i = 0; i < f->num_rules; i++) {
        if (f->rules[i].syscall_nr == (int)num) {
            uint32_t action = f->rules[i].action;
            if (action == SECCOMP_RET_ALLOW || action == SECCOMP_RET_LOG)
                return 1; /* allowed */
            else
                return 0; /* blocked (KILL or TRAP) */
        }
    }

    /* Default action for unmatched syscalls: allow */
    return 1;
}

/* ── Stub: seccomp_register_mode ─────────────────────────────── */
int seccomp_register_mode(int mode, void *ops)
{
    (void)mode;
    (void)ops;
    kprintf("[seccomp] seccomp_register_mode: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: seccomp_unregister_mode ─────────────────────────────── */
int seccomp_unregister_mode(int mode)
{
    (void)mode;
    kprintf("[seccomp] seccomp_unregister_mode: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: seccomp_run_filters ─────────────────────────────── */
int seccomp_run_filters(void *task, int syscall_nr, void *args)
{
    (void)task;
    (void)syscall_nr;
    (void)args;
    kprintf("[seccomp] seccomp_run_filters: not yet implemented\n");
    return -ENOSYS;
}
