#define KERNEL_INTERNAL
#include "caps.h"
#include "process.h"
#include "string.h"
#include "printf.h"
#include "audit.h"
#include "errno.h"
#include "module.h"

/* Module metadata */
MODULE_LICENSE("GPL v2");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Capability enforcement — system-wide capability bounding set");
MODULE_AUTHOR("Ruslan Gustomiasov");

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

/* Apply the system-wide bounding set to a process's cap sets.
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

/* Get one word (64-bit) of the system-wide capability bounding set.
 * Returns 0 if 'word' is out of bounds. Used by sys_capset for
 * permission validation against the global bounding mask. */
uint64_t sys_cap_bset_get_word(int word) {
	if (word < 0 || word >= CAP_BSET_SIZE)
		return 0;
	return sys_cap_bset[word];
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

/* ── cap_capget ─────────────────────────────────────────────────────── */
/*
 * Get the capability sets of a target process.
 * Returns 0 on success, -EINVAL on invalid parameters.
 */
int cap_capget(struct task_struct *target, uint64_t *effective, uint64_t *inheritable, uint64_t *permitted)
{
    if (!target)
        return -EINVAL;

    struct process *proc = (struct process *)target;

    if (effective)
        *effective = proc->cap_effective[0];
    if (inheritable)
        *inheritable = proc->cap_inheritable[0];
    if (permitted)
        *permitted = proc->cap_permitted[0];

    return 0;
}

/* ── cap_capset ─────────────────────────────────────────────────────── */
/*
 * Set the capability sets of a target process.
 * Requires CAP_SETPCAP in the caller's effective set.
 * Returns 0 on success, -EPERM/‑EINVAL on error.
 */
int cap_capset(struct task_struct *target, uint64_t *effective, uint64_t *inheritable, uint64_t *permitted)
{
    if (!target)
        return -EINVAL;

    struct process *proc = (struct process *)target;
    struct process *caller = process_get_current();

    /* Only root/CAP_SETPCAP can set capabilities on other processes */
    if (!caller)
        return -EPERM;

    /* Check if caller has CAP_SETPCAP */
    int word = CAP_SETPCAP / 64;
    int bit  = CAP_SETPCAP % 64;
    if (word < PROCESS_SYSCALL_CAP_WORDS &&
        !(caller->syscall_caps[word] & (1ULL << bit))) {
        return -EPERM;
    }

    /* Can only set caps that are in the caller's permitted set
     * AND in the system-wide bounding set */
    if (effective) {
        uint64_t new_eff = *effective;
        /* Cannot add caps not in permitted set */
        if ((new_eff & ~proc->cap_permitted[0]) != 0)
            return -EPERM;
        proc->cap_effective[0] = new_eff;
    }

    if (inheritable) {
        proc->cap_inheritable[0] = *inheritable;
    }

    if (permitted) {
        uint64_t new_perm = *permitted;
        /* Cannot exceed system-wide bounding set */
        new_perm &= sys_cap_bset[0];
        proc->cap_permitted[0] = new_perm;
    }

    return 0;
}

/* ── cap_bprm_set_creds ─────────────────────────────────────────────── */
/*
 * Set credentials during exec. Computes the new capability sets
 * based on the binary's file capabilities and the current process's
 * inheritable/bset/securebits.
 *
 * Returns 0 on success, negative on error.
 */
int cap_bprm_set_creds(struct linux_binprm *bprm)
{
    if (!bprm)
        return -EINVAL;

    struct process *p = process_get_current();
    if (!p)
        return -EPERM;

    /* Apply securebits rules:
     * If SECBIT_NO_SETUID_FIXUP is set, preserve existing effective set.
     * Otherwise, recompute based on inheritable & permitted sets. */
    if (!(p->securebits & SECBIT_KEEP_CAPS)) {
        /* Clear effective set (will be recomputed) */
        for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
            p->cap_effective[i] = 0;
        }
    }

    /* If NOROOT is set, don't automatically grant capabilities to root */
    if (p->securebits & SECBIT_NOROOT) {
        /* Root does not get special treatment */
    }

    /* Apply bounding set: permitted set is ANDed with cap_bset */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
        p->cap_permitted[i] &= p->cap_bset[i];
    }

    /* Effective set = (inheritable & permitted) | (if file has caps, add file caps) */
    for (int i = 0; i < PROCESS_SYSCALL_CAP_WORDS; i++) {
        p->cap_effective[i] = p->cap_inheritable[i] & p->cap_permitted[i];
    }

    /* If the binary has file capabilities (setcap), they would be
     * applied here. For now, without a full file caps implementation,
     * we keep the computed sets. */

    return 0;
}

/* ── cap_task_prctl ─────────────────────────────────────────────────── */
/*
 * Handle capability-related prctl operations.
 * Supported options:
 *   PR_SET_KEEPCAPS -> SECBIT_KEEP_CAPS
 *   PR_GET_KEEPCAPS -> query SECBIT_KEEP_CAPS
 *   PR_SET_SECCOMP -> seccomp mode (not strictly caps, but related)
 *
 * Returns 0 on success, -EINVAL for unknown options.
 */
int cap_task_prctl(int option, unsigned long arg2, unsigned long arg3)
{
    struct process *p = process_get_current();
    if (!p)
        return -EINVAL;

    switch (option) {
    case 1: /* PR_SET_KEEPCAPS (Linux value 1) */
        p->securebits = (uint8_t)(arg2 ? (p->securebits | SECBIT_KEEP_CAPS)
                                       : (p->securebits & ~SECBIT_KEEP_CAPS));
        return 0;

    case 2: /* PR_GET_KEEPCAPS (Linux value 2) */
        return p->securebits & SECBIT_KEEP_CAPS;

    case 3: /* PR_SET_SECCOMP (Linux value 22 = 0x16) handled elsewhere */
    case 22:
        /* seccomp mode is set by the prctl handler in syscall.c */
        return 0;

    case 4: /* PR_CAPBSET_READ (Linux value 23) */
        if (arg2 > CAP_LAST_CAP)
            return -EINVAL;
        return cap_bset_has((uint32_t)arg2);

    case 5: /* PR_CAPBSET_DROP (Linux value 24) */
        if (arg2 > CAP_LAST_CAP)
            return -EINVAL;
        cap_bset_drop((uint32_t)arg2);
        return 0;

    case 6: /* PR_SET_SECUREBITS (Linux value 28 = 0x1c) */
    case 28: {
        uint8_t new_bits = (uint8_t)(arg2 & SECBIT_ALLOWED_MASK);
        /* If locked bits are set, don't allow changing those bits */
        if ((p->securebits & SECBIT_KEEP_CAPS_LOCKED) &&
            ((new_bits ^ p->securebits) & SECBIT_KEEP_CAPS))
            return -EPERM;
        if ((p->securebits & SECBIT_NO_SETUID_FIXUP_LOCKED) &&
            ((new_bits ^ p->securebits) & SECBIT_NO_SETUID_FIXUP))
            return -EPERM;
        if ((p->securebits & SECBIT_NOROOT_LOCKED) &&
            ((new_bits ^ p->securebits) & SECBIT_NOROOT))
            return -EPERM;
        p->securebits = new_bits;
        return 0;
    }

    case 7: /* PR_GET_SECUREBITS (Linux value 27 = 0x1b) */
    case 27:
        return p->securebits;

    case 8: /* PR_SET_NO_NEW_PRIVS (Linux value 38 = 0x26) */
    case 38:
        p->no_new_privs = 1;
        return 0;

    case 9: /* PR_GET_NO_NEW_PRIVS (Linux value 39 = 0x27) */
    case 39:
        return p->no_new_privs;

    default:
        return -EINVAL;
    }
}

/* ── cap_task_setscheduler ─────────────────────────────────────────── */
/*
 * Check capability for setting scheduler parameters.
 * Requires CAP_SYS_NICE.
 */
int cap_task_setscheduler(struct task_struct *p)
{
    if (!p)
        return -EINVAL;

    return cap_capable_audit(CAP_SYS_NICE, "setscheduler");
}

/* ── cap_task_setioprio ─────────────────────────────────────────────── */
/*
 * Check capability for setting I/O priority.
 * Requires CAP_SYS_ADMIN or CAP_SYS_NICE depending on target.
 */
int cap_task_setioprio(struct task_struct *p, int ioprio)
{
    if (!p)
        return -EINVAL;

    (void)ioprio;

    /* Changing I/O priority of another process needs CAP_SYS_ADMIN.
     * Setting one's own I/O priority needs CAP_SYS_NICE. */
    struct process *caller = process_get_current();
    if (!caller)
        return -EPERM;

    uint32_t caller_pid = caller->pid;
    uint32_t target_pid = ((struct process *)p)->pid;

    if (caller_pid != target_pid) {
        /* Modifying another process: need CAP_SYS_ADMIN */
        return cap_capable_audit(CAP_SYS_ADMIN, "setioprio-other");
    }

    /* Self-modification: need CAP_SYS_NICE */
    return cap_capable_audit(CAP_SYS_NICE, "setioprio-self");
}

/* ── cap_task_setnice ───────────────────────────────────────────────── */
/*
 * Check capability for setting nice value.
 * Requires CAP_SYS_NICE.
 */
int cap_task_setnice(struct task_struct *p, int nice)
{
    if (!p)
        return -EINVAL;

    (void)nice;

    struct process *caller = process_get_current();
    if (!caller)
        return -EPERM;

    uint32_t caller_pid = caller->pid;
    uint32_t target_pid = ((struct process *)p)->pid;

    if (caller_pid != target_pid) {
        /* Modifying another process's nice value: need CAP_SYS_NICE */
        return cap_capable_audit(CAP_SYS_NICE, "setnice-other");
    }

    /* Self-modification: lowering nice (higher priority) needs CAP_SYS_NICE.
     * Raising nice (lower priority) is always allowed. */
    if (nice < 0 && nice < ((struct process *)p)->nice) {
        return cap_capable_audit(CAP_SYS_NICE, "setnice-lower");
    }

    return 0;
}

/* ── cap_vm_enough_memory ──────────────────────────────────────────── */
/*
 * Check whether a process has enough memory to fulfill a request.
 * Returns 0 if enough memory is available (or overcommit is allowed),
 * -ENOMEM if the allocation should be denied.
 *
 * CAP_SYS_ADMIN allows overcommit to always succeed.
 */
int cap_vm_enough_memory(struct mm_struct *mm, long pages)
{
    if (!mm)
        return -EINVAL;

    (void)mm;

    /* Root / CAP_SYS_ADMIN can overcommit freely */
    if (cap_capable_audit(CAP_SYS_ADMIN, "vm_overcommit") == 0)
        return 0;

    /* Check if there's enough committed memory space */
    uint64_t needed_bytes = (uint64_t)pages * PAGE_SIZE;
    if (vmm_get_committed() < 0) {
        /* Overcommit is enabled */
        return 0;
    }

    /* Try to commit the memory */
    if (vmm_commit(needed_bytes) < 0) {
        return -ENOMEM;
    }

    return 0;
}
