/*
 * wx_enforce.c — W^X (Write XOR Execute) enforcement
 *
 * Prevents memory mappings from being simultaneously writable and
 * executable.  This is a fundamental security hardening measure that
 * blocks shellcode injection via writable memory.
 *
 * The policy is controlled by the sysctl vm.mmap_wx_enabled:
 *   0 = W^X enforced (default, secure)
 *   1 = W^X relaxed (legacy compatibility, dangerous)
 *
 * Integration points:
 *   - sys_mmap()     in syscall.c checks via wx_enforce_check_prot()
 *   - sys_mprotect() in mprotect.c checks via wx_enforce_check()
 *   - vmm_map_page() in vmm.c checks via wx_enforce_check()
 */

#define KERNEL_INTERNAL
#include "wx_enforce.h"
#include "printf.h"
#include "errno.h"
#include "sysctl.h"

/* ── Global toggle (extern in wx_enforce.h) ──────────────────────────
 * 0 = W^X enforced (default, deny W+X).
 * 1 = W^X relaxed (allow W+X). */
int wx_enabled = 0;

/* ── Sysctl read/write handlers ─────────────────────────────────────
 * These implement /proc/sys/vm/mmap_wx_enabled. */

static int wx_sysctl_read(char *buf, int max) {
    return snprintf(buf, (size_t)max, "%d\n", wx_enabled);
}

static int wx_sysctl_write(const char *buf, int len) {
    if (len < 1) return -EINVAL;
    int val = 0;
    /* Parse integer from string */
    for (int i = 0; i < len && buf[i] >= '0' && buf[i] <= '9'; i++) {
        val = val * 10 + (buf[i] - '0');
    }
    if (val < 0 || val > 1) return -EINVAL;
    wx_enabled = val;
    kprintf("[W^X] sysctl vm.mmap_wx_enabled set to %d\n", wx_enabled);
    return len;
}

/* ── Initialisation ──────────────────────────────────────────────── */

void wx_enforce_init(void) {
    /* Register the sysctl entry */
    int ret = sysctl_register("vm.mmap_wx_enabled",
                               wx_sysctl_read,
                               wx_sysctl_write);
    if (ret == 0) {
        kprintf("[W^X] Enforcement active (wx_enabled=%d, default deny W+X)\n",
                wx_enabled);
    } else {
        kprintf("[W^X] WARNING: failed to register sysctl (ret=%d)\n", ret);
    }
}

/* ── Flag-based check (for vmm_map_page, vmm_set_user_pages_flags) ─ */

int wx_enforce_check(uint64_t vm_flags) {
    /* If W^X is relaxed, allow anything */
    if (wx_enabled)
        return 0;

    /* W^X denied: reject if both WRITE and EXEC (no NX) are present.
     * The NOEXEC flag (bit 63) is set for non-executable pages.
     * If WRITE is set and NOEXEC is NOT set → W+X → reject. */
    if ((vm_flags & VMM_FLAG_WRITE) && !(vm_flags & VMM_FLAG_NOEXEC)) {
        return -EPERM;
    }

    return 0;
}

/* ── POSIX PROT_* flag-based check (for sys_mmap, sys_mprotect) ──── */

int wx_enforce_check_prot(uint64_t prot) {
    /* If W^X is relaxed, allow anything */
    if (wx_enabled)
        return 0;

    /* W^X denied: reject if both PROT_WRITE and PROT_EXEC are set */
    if ((prot & PROT_WRITE) && (prot & PROT_EXEC)) {
        return -EPERM;
    }

    return 0;
}

/* ── Stub: wx_enforce_apply ─────────────────────────────── */
int wx_enforce_apply(void *task)
{
    (void)task;
    kprintf("[wx] wx_enforce_apply: not yet implemented\n");
    return -ENOSYS;
}
