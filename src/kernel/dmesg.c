/* dmesg.c — dmesg_restrict */

#define KERNEL_INTERNAL
#include "dmesg.h"
#include "printf.h"
#include "process.h"
#include "sysctl.h"
#include "string.h"

/* dmesg_restrict: when set, only root can read dmesg */
int __read_mostly dmesg_restrict = 1;

/* ── Sysctl handlers ──────────────────────────────────────────────── */

static int sysctl_read_dmesg_restrict(char *buf, int max) {
    if (max < 3) return 0;
    buf[0] = '0' + (char)dmesg_restrict;
    buf[1] = '\n';
    buf[2] = '\0';
    return 2;
}

static int sysctl_write_dmesg_restrict(const char *buf, int len) {
    if (len > 0 && buf[0] >= '0' && buf[0] <= '1')
        dmesg_restrict = buf[0] - '0';
    return 0;
}

/* ── API ──────────────────────────────────────────────────────────── */

void __init dmesg_init(void) {
    kprintf("[OK] dmesg_restrict initialized (value=%d)\n", dmesg_restrict);

    /* Register sysctl entry: kernel.dmesg_restrict */
    sysctl_register("dmesg_restrict", sysctl_read_dmesg_restrict, sysctl_write_dmesg_restrict);
}

int dmesg_check_access(void) {
    if (!dmesg_restrict)
        return 1;  /* unrestricted: allow */

    struct process *p = process_get_current();
    if (!p) return 1;  /* kernel thread: allow */

    if (p->euid == 0 || p->uid == 0)
        return 1;  /* root: allow */

    return 0;  /* non-root: deny */
}

/* ── Stub: dmesg_read ─────────────────────────────── */
static int dmesg_read(void *buf, size_t count, uint64_t *pos)
{
    (void)buf;
    (void)count;
    (void)pos;
    kprintf("[dmesg] dmesg_read: not yet implemented\n");
    return 0;
}
/* ── Stub: dmesg_write ─────────────────────────────── */
static int dmesg_write(const void *buf, size_t count)
{
    (void)buf;
    (void)count;
    kprintf("[dmesg] dmesg_write: not yet implemented\n");
    return 0;
}
/* dmesg_clear: implemented in lib/printf.c */
