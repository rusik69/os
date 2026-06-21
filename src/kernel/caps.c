#define KERNEL_INTERNAL
#include "caps.h"
#include "process.h"
#include "string.h"
#include "printf.h"
#include "audit.h"
#include "errno.h"

/*
 * System-wide capability bounding set — limits what capabilities
 * any process on the system can ever acquire.
 *
 * This is the global (admin-configurable) bounding set, separate
 * from the per-process bounding set in process.c.  On fork and
 * exec the kernel ANDs the per-process bounding set with this
 * global mask, ensuring that no process can gain a capability
 * that has been dropped system-wide.
 */

/* Global bounding set — static to this file, accessed via API */
static uint64_t sys_cap_bset[CAP_BSET_SIZE];

void sys_cap_bset_init(void) {
    memset(sys_cap_bset, 0, sizeof(sys_cap_bset));
    /* By default, allow all POSIX capabilities */
    for (int i = 0; i < CAP_BSET_SIZE; i++)
        sys_cap_bset[i] = ~0ULL;
    kprintf("[OK] sys_cap_bset initialized (global bounding set)\n");
}

/* Drop a capability from the system-wide bounding set.
 * Once dropped, no process can ever re-acquire this capability. */
void sys_cap_bset_drop(uint32_t cap) {
    if (cap > CAP_LAST_CAP) return;
    int word = cap / 64;
    int bit = cap % 64;
    if (word < CAP_BSET_SIZE) {
        sys_cap_bset[word] &= ~(1ULL << bit);
    }
}

/* Check if a capability is present in the system-wide bounding set */
int sys_cap_bset_has(uint32_t cap) {
    if (cap > CAP_LAST_CAP) return 0;
    int word = cap / 64;
    int bit = cap % 64;
    if (word >= CAP_BSET_SIZE) return 0;
    return (sys_cap_bset[word] >> bit) & 1;
}

/* Apply the system-wide bounding set mask to per-process cap sets.
 * Called during fork/clone and exec to ensure no process exceeds
 * the global bounding set limits. */
void sys_cap_bset_apply(struct process *proc) {
    if (!proc) return;
    /* The per-process syscall_caps (permitted set) is ANDed with
     * the global bounding set so dropping a cap at system level
     * immediately restricts all processes. */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS && i < CAP_BSET_SIZE; i++) {
        proc->syscall_caps[i] &= sys_cap_bset[i];
        proc->cap_bset[i]     &= sys_cap_bset[i];
    }
}

/*
 * ══════════════════════════════════════════════════════════════════════════
 * Capability-aware audit enforcement
 * ══════════════════════════════════════════════════════════════════════════
 *
 * These functions check the current process's effective capability set
 * and log audit events on denial. The caps_capable() from kaps.c is the
 * low-level bit check; these wrappers add audit trail and errno returns.
 */

/* Check if the current process has a specific capability.
 * Returns 0 if granted, -EPERM if denied (logs audit event).
 * @cap:   The capability number (CAP_* constant)
 * @audit_msg:  Descriptive string for the audit log (e.g. "ioperm")
 */
int cap_capable_audit(uint32_t cap, const char *audit_msg)
{
    struct process *p = process_get_current();

    /* Kernel context always has capabilities */
    if (!p || !p->is_user)
        return 0;

    /* Check per-process syscall caps (effective set) */
    int word = cap / 64;
    int bit  = cap % 64;
    if (word >= PROCESS_SYSCALL_CAP_WORDS)
        return -EPERM;

    if (p->syscall_caps[word] & (1ULL << bit))
        return 0;  /* granted */

    /* Denied — log audit event */
    audit_log_denial(audit_msg ? audit_msg : "unknown",
                     "capability", "want_cap");
    return -EPERM;
}

/* Convenience wrapper for CAP_SYS_RAWIO checks (ioperm, iopl, port I/O, /dev/mem) */
int cap_sys_rawio_check(void)
{
    return cap_capable_audit(CAP_SYS_RAWIO, "cap_sys_rawio");
}

/* Convenience wrapper for CAP_SYS_BOOT checks (kexec_load) */
int cap_sys_boot_check(void)
{
    return cap_capable_audit(CAP_SYS_BOOT, "cap_sys_boot");
}

/* Convenience wrapper for CAP_SYS_MODULE checks (init_module, finit_module) */
int cap_sys_module_check(void)
{
    return cap_capable_audit(CAP_SYS_MODULE, "cap_sys_module");
}

/* Forward declarations for stub functions */
struct task_struct;
struct linux_binprm;
struct mm_struct;

/* ── Stub: cap_capget ─────────────────────────────── */
int cap_capget(struct task_struct *target, uint64_t *effective, uint64_t *inheritable, uint64_t *permitted)
{
    (void)target;
    (void)effective;
    (void)inheritable;
    (void)permitted;
    kprintf("[caps] cap_capget: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_capset ─────────────────────────────── */
int cap_capset(struct task_struct *target, uint64_t *effective, uint64_t *inheritable, uint64_t *permitted)
{
    (void)target;
    (void)effective;
    (void)inheritable;
    (void)permitted;
    kprintf("[caps] cap_capset: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_bprm_set_creds ─────────────────────────────── */
int cap_bprm_set_creds(struct linux_binprm *bprm)
{
    (void)bprm;
    kprintf("[caps] cap_bprm_set_creds: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_task_prctl ─────────────────────────────── */
int cap_task_prctl(int option, unsigned long arg2, unsigned long arg3)
{
    (void)option;
    (void)arg2;
    (void)arg3;
    kprintf("[caps] cap_task_prctl: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_task_setscheduler ─────────────────────────────── */
int cap_task_setscheduler(struct task_struct *p)
{
    (void)p;
    kprintf("[caps] cap_task_setscheduler: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_task_setioprio ─────────────────────────────── */
int cap_task_setioprio(struct task_struct *p, int ioprio)
{
    (void)p;
    (void)ioprio;
    kprintf("[caps] cap_task_setioprio: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_task_setnice ─────────────────────────────── */
int cap_task_setnice(struct task_struct *p, int nice)
{
    (void)p;
    (void)nice;
    kprintf("[caps] cap_task_setnice: not yet implemented\n");
    return -ENOSYS;
}

/* ── Stub: cap_vm_enough_memory ─────────────────────────────── */
int cap_vm_enough_memory(struct mm_struct *mm, long pages)
{
    (void)mm;
    (void)pages;
    kprintf("[caps] cap_vm_enough_memory: not yet implemented\n");
    return -ENOSYS;
}
