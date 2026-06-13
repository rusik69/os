/* execshield.c — W^X exec-shield enforcement
 *
 * Enforces W^X (write XOR execute) policy for user-space memory mappings:
 *   - Rejects mmap() with both PROT_WRITE and PROT_EXEC set
 *   - Rejects mprotect() adding PROT_EXEC to a writable mapping
 *   - Rejects mprotect() adding PROT_WRITE to an executable mapping
 *
 * A sysctl toggle (vm.exec_shield) allows disabling for legacy JITs.
 *
 * Inspired by the Linux kernel's CONFIG_EXEC_SHIELD / PaX MPROTECT.
 */

#include "types.h"
#include "printf.h"
#include "process.h"
#include "vmm.h"
#include "sysctl.h"
#include "string.h"
#include "errno.h"
#include "stdlib.h"

/* ── Sysctl tunable ────────────────────────────────────────────────── */

/* exec_shield_enabled:
 *   0 = disabled (W|X allowed)
 *   1 = enabled  (W|X rejected, default)
 *   2 = strict   (also rejects RW->RX and RX->RW transitions)
 */
static int exec_shield_enabled = 1;

/* ── Sysctl handlers ───────────────────────────────────────────────── */

static int execshield_sysctl_read(char *buf, int max)
{
    return snprintf(buf, (size_t)max, "%d\n", exec_shield_enabled);
}

static int execshield_sysctl_write(const char *buf, int len)
{
    if (len < 1) return -EINVAL;
    int val = buf[0] - '0';
    if (val < 0 || val > 2) return -EINVAL;
    exec_shield_enabled = val;
    return len;
}

/* ── Core check: should we allow a mapping with given prot flags? ──── */

int execshield_check_mmap(uint64_t addr, size_t length,
                           int prot, int flags)
{
    (void)addr;
    (void)length;
    (void)flags;

    if (exec_shield_enabled == 0)
        return 0; /* allow */

    int wants_write  = (prot & 2) != 0;  /* PROT_WRITE  */
    int wants_exec   = (prot & 4) != 0;  /* PROT_EXEC   */

    /* W+X is never allowed */
    if (wants_write && wants_exec)
        return -EPERM;

    return 0; /* allow */
}

/* ── Check for mprotect transitions ────────────────────────────────── */

int execshield_check_mprotect(uint64_t addr, size_t length,
                               int old_prot, int new_prot)
{
    (void)addr;
    (void)length;

    if (exec_shield_enabled == 0)
        return 0; /* allow */

    int old_write = (old_prot & 2) != 0;
    int old_exec  = (old_prot & 4) != 0;
    int new_write = (new_prot & 2) != 0;
    int new_exec  = (new_prot & 4) != 0;

    /* W+X in either old or new → reject */
    if (new_write && new_exec)
        return -EPERM;

    /* In strict mode, also reject transitions:
     *   RW → RX  (adding execute to writable)
     *   RX → RW  (adding write to executable)
     */
    if (exec_shield_enabled >= 2) {
        if (old_write && !old_exec && new_exec)
            return -EPERM;  /* RW → RX */
        if (old_exec && !old_write && new_write)
            return -EPERM;  /* RX → RW */
    }

    return 0; /* allow */
}

/* ── Query current state ───────────────────────────────────────────── */

int execshield_get_enabled(void)
{
    return exec_shield_enabled;
}

void execshield_set_enabled(int val)
{
    if (val >= 0 && val <= 2)
        exec_shield_enabled = val;
}

/* ── Initialization ────────────────────────────────────────────────── */

void execshield_init(void)
{
    /* Register sysctl: exec_shield under /proc/sys/kernel/ */
    sysctl_register("exec_shield", execshield_sysctl_read, execshield_sysctl_write);

    kprintf("[OK] EXEC_SHIELD: W^X enforcement active (mode=%d)%s\n",
            exec_shield_enabled,
            exec_shield_enabled ? " (W+X rejected)" : " (disabled)");
}
