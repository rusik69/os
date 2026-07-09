#define KERNEL_INTERNAL
#include "signalfd.h"
#include "printf.h"
#include "types.h"
#include "string.h"
#include "signal.h"

/*
 * signalfd.c — Signal file descriptor subsystem init
 *
 * The core signalfd logic (sys_signalfd, signalfd_do_read,
 * signalfd_notify, signalfd_notify_ext) lives in kernel/syscall.c.
 *
 * This file provides the boot-time init hook.
 * Kernel-internal callers should use signalfd_create() which is
 * defined here and accesses the signalfd subsystem via the
 * signalfd_table directly.
 */

/* The signalfd table is defined in syscall.c — declare it extern */
#define SIGNALFD_MAX 16
#define SIGNALFD_BUF 8

struct signalfd_info {
    int      in_use;
    uint32_t sigmask;
    struct siginfo ring[8];
    int      head;
    int      tail;
    int      count;
};

extern struct signalfd_info signalfd_table[];

void signalfd_init(void)
{
    kprintf("[OK] signalfd subsystem initialized (siginfo extraction ready)\n");
}

/*
 * Create a signalfd with the given mask.
 * Returns the signalfd fd number (600-based), or -1 on failure.
 */
int signalfd_create(uint64_t mask)
{
    for (int i = 0; i < SIGNALFD_MAX; i++) {
        if (!signalfd_table[i].in_use) {
            signalfd_table[i].in_use = 1;
            signalfd_table[i].sigmask = (uint32_t)mask;
            signalfd_table[i].head = 0;
            signalfd_table[i].tail = 0;
            signalfd_table[i].count = 0;
            memset(signalfd_table[i].ring, 0, sizeof(signalfd_table[i].ring));
            return 600 + i;
        }
    }
    return -1;
}

/* ── Stub: signalfd_read ─────────────────────────────── */
static int signalfd_read(__maybe_unused int fd, __maybe_unused void *buf, __maybe_unused size_t count)
{
    kprintf("[SIGNALFD] signalfd_read: not yet implemented\n");
    return 0;
}
/* ── Stub: signalfd_poll ─────────────────────────────── */
int signalfd_poll(__maybe_unused int fd, __maybe_unused void *pt)
{
    kprintf("[SIGNALFD] signalfd_poll: not yet implemented\n");
    return 0;
}
