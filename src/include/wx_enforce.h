#ifndef WX_ENFORCE_H
#define WX_ENFORCE_H

#include "types.h"
#include "vmm.h"

/*
 * W^X Enforcement — Prevent writable + executable mappings
 *
 * W^X ("Write XOR Execute") is a security policy that prohibits memory
 * pages from being simultaneously writable AND executable.  This prevents
 * an attacker from writing shellcode to a page then executing it.
 *
 * The policy is enforced at two points:
 *   1. mmap()    — vmm_map_user_pages / sys_mmap checks via W^X check
 *   2. mprotect() — sys_mprotect checks before changing page permissions
 *
 * The sysctl vm.mmap_wx_enabled controls the mode:
 *   0 (default)  — W^X denied: reject any mapping with both W and X
 *   1            — W^X allowed: permit W+X (legacy compat, dangerous)
 *
 * When W^X is denied, a call to mmap(PROT_WRITE|PROT_EXEC) or
 * mprotect(PROT_WRITE|PROT_EXEC) returns -EACCES.
 */

/* ── Sysctl tunable ──────────────────────────────────────────────── */
extern int wx_enabled;  /* 0 = deny W+X (default), 1 = allow W+X */

/* ── Initialisation ──────────────────────────────────────────────── */

/**
 * wx_enforce_init - Initialize W^X enforcement subsystem.
 * Registers the vm.mmap_wx_enabled sysctl entry.
 * Called once during kernel boot.
 */
void wx_enforce_init(void);

/* ── Policy check ────────────────────────────────────────────────── */

/**
 * wx_enforce_check - Check if the requested page flags violate W^X policy.
 *
 * @vm_flags:  VMM page flags (VMM_FLAG_WRITE, VMM_FLAG_PRESENT, etc.)
 *
 * Returns: 0 if the mapping is allowed under current W^X policy.
 *          -EPERM if W+X is requested and W^X is enforced.
 *
 * The check is: if VMM_FLAG_WRITE is set AND VMM_FLAG_NOEXEC is NOT set
 * (i.e., executable), AND wx_enabled == 0, then W+X is denied.
 */
int wx_enforce_check(uint64_t vm_flags);

/**
 * wx_enforce_check_prot - Check POSIX PROT_* flags against W^X policy.
 *
 * @prot: POSIX protection flags (PROT_READ|PROT_WRITE|PROT_EXEC)
 *
 * Returns: 0 if allowed, -EPERM if W+X is denied.
 */
int wx_enforce_check_prot(uint64_t prot);

/* ── Query ───────────────────────────────────────────────────────── */

/**
 * wx_enforce_is_active - Returns 1 if W^X is enforced (wx_enabled == 0).
 */
static inline int wx_enforce_is_active(void) {
    extern int wx_enabled;
    return wx_enabled == 0;
}

#endif /* WX_ENFORCE_H */
