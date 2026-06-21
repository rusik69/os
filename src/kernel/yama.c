/* yama.c — YAMA ptrace security
 *
 * Implements three ptrace scope levels:
 *   0 (disabled)  — any process can trace any other (no restriction)
 *   1 (restricted) — tracer must be a descendant of the target
 *   2 (admin)     — tracer needs CAP_SYS_PTRACE AND target must have
 *                   called prctl(PR_SET_PTRACER, tracer_pid) to opt in
 *
 * Scope is controlled via sysctl /proc/sys/kernel/yama/ptrace_scope.
 */

#define KERNEL_INTERNAL
#include "yama.h"
#include "printf.h"
#include "process.h"
#include "caps.h"
#include "sysctl.h"

/* YAMA ptrace scope: 0 = disabled, 1 = restricted (descendants only),
 * 2 = admin-controlled (CAP_SYS_PTRACE + PR_SET_PTRACER opt-in). */
int yama_ptrace_scope = YAMA_PTRACE_SCOPE_RESTRICTED;

void yama_init(void) {
    kprintf("[OK] YAMA ptrace initialized (scope=%d)\n", yama_ptrace_scope);
}

/* Set the allowed tracer PID for a process (called from prctl PR_SET_PTRACER) */
void yama_set_ptracer(uint32_t target_pid, int tracer_pid) {
    struct process *target = process_get_by_pid(target_pid);
    if (!target) return;
    target->ptracer_pid = tracer_pid;
}

/* Get the allowed tracer PID for a process (called from prctl PR_GET_PTRACER) */
int yama_get_ptracer(uint32_t target_pid) {
    struct process *target = process_get_by_pid(target_pid);
    if (!target) return 0;
    return target->ptracer_pid;
}

/* Check whether @caller_pid is allowed to ptrace @target_pid.
 * Returns 1 if allowed, 0 if denied. */
int yama_ptrace_allowed(uint32_t caller_pid, uint32_t target_pid) {
    /* Scope 0: allow all */
    if (yama_ptrace_scope == YAMA_PTRACE_SCOPE_DISABLED)
        return 1;

    struct process *caller = process_get_by_pid(caller_pid);
    struct process *target = process_get_by_pid(target_pid);
    if (!caller || !target) return 0;

    /* Scope 1 and 2: check descendant relationship first
     * (a process can always trace its own children) */
    struct process *p = target;
    while (p && p->parent_pid != p->pid) {
        if (p->parent_pid == caller_pid)
            return 1;
        p = process_get_by_pid(p->parent_pid);
    }

    /* Scope 1: only descendants allowed (already checked above) */
    if (yama_ptrace_scope == YAMA_PTRACE_SCOPE_RESTRICTED)
        return 0;

    /* Scope 2: admin-controlled — check PR_SET_PTRACER + CAP_SYS_PTRACE */
    if (yama_ptrace_scope == YAMA_PTRACE_SCOPE_ADMIN) {
        /* Does the target explicitly allow ANY tracer? */
        if (target->ptracer_pid == PR_SET_PTRACER_PID_ANY)
            return 1;

        /* Does the target explicitly allow THIS specific tracer? */
        if (target->ptracer_pid > 0 && (uint32_t)target->ptracer_pid == caller_pid)
            return 1;

        /* Target hasn't opted in — check CAP_SYS_PTRACE on caller.
         * If the caller has CAP_SYS_PTRACE, they can trace regardless. */
        if (process_caps_has(caller, CAP_SYS_PTRACE))
            return 1;

        return 0;
    }

    /* Unknown scope — deny to be safe */
    return 0;
}

/* ─── Sysctl handler for ptrace_scope ────────────────────────────── */

static int sysctl_read_ptrace_scope(char *buf, int max) {
    if (max < 3) return 0;
    buf[0] = '0' + (char)yama_ptrace_scope;
    buf[1] = '\n';
    buf[2] = '\0';
    return 2;
}

static int sysctl_write_ptrace_scope(const char *buf, int len) {
    if (len > 0 && buf[0] >= '0' && buf[0] <= '2')
        yama_ptrace_scope = buf[0] - '0';
    return 0;
}

/* Called during kernel boot to register the YAMA sysctl entry */
void yama_sysctl_register(void) {
    sysctl_register("yama.ptrace_scope",
                    sysctl_read_ptrace_scope,
                    sysctl_write_ptrace_scope);
}
/* Forward declarations for stub functions */
struct task_struct;

/* ── Stub: yama_ptrace_access_check ─────────────────────────────── */
int yama_ptrace_access_check(struct task_struct *child, unsigned int mode)
{
    (void)child;
    (void)mode;
    kprintf("[yama] yama_ptrace_access_check: not yet implemented\n");
    return 0;
}

/* ── Stub: yama_ptrace_traceme_allowed ─────────────────────────────── */
int yama_ptrace_traceme_allowed(struct task_struct *parent)
{
    (void)parent;
    kprintf("[yama] yama_ptrace_traceme_allowed: not yet implemented\n");
    return 0;
}

/* ── Stub: yama_task_prctl ─────────────────────────────── */
int yama_task_prctl(int option, unsigned long arg2, unsigned long arg3)
{
    (void)option;
    (void)arg2;
    (void)arg3;
    kprintf("[yama] yama_task_prctl: not yet implemented\n");
    return 0;
}

/* ── Stub: yama_task_free ─────────────────────────────── */
void yama_task_free(struct task_struct *task)
{
    (void)task;
    kprintf("[yama] yama_task_free: not yet implemented\n");
}

#include "module.h"
module_init(yama_init);
