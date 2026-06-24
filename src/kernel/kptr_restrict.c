/* kptr_restrict.c — Kernel pointer restrict */

#define KERNEL_INTERNAL
#include "kptr_restrict.h"
#include "printf.h"
#include "process.h"
#include "sysctl.h"
#include "string.h"

/* kptr_restrict:
 *   0 = show all kernel addresses,
 *   1 = hide from non-root,
 *   2 = hide from everyone (including root) */
int __read_mostly kptr_restrict = KPTR_RESTRICT_RESTRICTED;

/* ── Sysctl handlers ──────────────────────────────────────────────── */

static int sysctl_read_kptr_restrict(char *buf, int max) {
    if (max < 3) return 0;
    buf[0] = '0' + (char)kptr_restrict;
    buf[1] = '\n';
    buf[2] = '\0';
    return 2;
}

static int sysctl_write_kptr_restrict(const char *buf, int len) {
    /* Only root (uid 0) may change kptr_restrict */
    struct process *p = process_get_current();
    if (p && p->euid != 0 && p->uid != 0)
        return -1;
    if (len > 0 && buf[0] >= '0' && buf[0] <= '2')
        kptr_restrict = buf[0] - '0';
    return 0;
}

/* ── API ──────────────────────────────────────────────────────────── */

void kptr_restrict_init(void) {
    kprintf("[OK] kptr_restrict initialized (value=%d)\n", kptr_restrict);

    /* Register sysctl entry: kernel.kptr_restrict */
    sysctl_register("kptr_restrict", sysctl_read_kptr_restrict, sysctl_write_kptr_restrict);
}

int kptr_restrict_check(void) {
    if (kptr_restrict == KPTR_RESTRICT_DISABLED)
        return 0;  /* show all */

    /* Level 2: hide from everyone (including root) */
    if (kptr_restrict >= KPTR_RESTRICT_ROOT_HIDE)
        return 1;  /* hide all */

    /* Level 1: Restricted — check if caller is root (uid 0) */
    struct process *p = process_get_current();
    if (!p) return 0;  /* kernel thread: show */

    if (p->euid == 0 || p->uid == 0)
        return 0;  /* root: show */

    return 1;  /* hide */
}

/* ── Stub: kptr_restrict_set ─────────────────────────────── */
int kptr_restrict_set(int level)
{
    (void)level;
    kprintf("[kptr] kptr_restrict_set: not yet implemented\n");
    return 0;
}
/* ── Stub: kptr_restrict_get ─────────────────────────────── */
int kptr_restrict_get(void)
{
    kprintf("[kptr] kptr_restrict_get: not yet implemented\n");
    return 0;
}
