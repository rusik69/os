/* cmd_init.c — 'init' command: switch system runlevel
 *
 * Item U4: Runlevel switching.
 *
 * Usage:
 *   init N     — switch to runlevel N (0-9)
 *   init       — show usage
 *
 * Writes the target runlevel to /var/run/initpipe, which PID 1 (init)
 * periodically reads to apply runlevel transitions.  On runlevels 0 (halt)
 * and 6 (reboot), init will terminate all services and signal shutdown.
 *
 * Runlevel semantics (SysV convention):
 *   0 = halt, 1 = single-user, 2 = multi-user (default),
 *   3 = networking, 4 = reserved, 5 = graphical, 6 = reboot
 *
 * Implementation: Uses raw syscalls for file I/O to avoid depending on
 * kernel-internal headers like fs.h.
 */

#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"
#include "syscall.h"

/* Path to the init control pipe -- must match src/init/init.c */
#define INITPIPE_PATH "/var/run/initpipe"

/* Open flags matching Linux ABI (used by init.c and here) */
#define O_RDONLY      0
#define O_WRONLY      1
#define O_CREAT       64
#define O_TRUNC       512

void cmd_init(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: init <runlevel> (0=halt,1=single,2=multi,3=net,5=graphical,6=reboot)\n");
        return;
    }

    /* Parse runlevel argument */
    const char *p = args;
    while (*p == ' ') p++;

    if (p[0] < '0' || p[0] > '9' || p[1] != '\0') {
        kprintf("init: invalid runlevel '%s' (use a single digit 0-9)\n", p);
        return;
    }

    int new_rl = p[0] - '0';

    kprintf("init: requesting runlevel %d...\n", new_rl);

    /* Write the runlevel to the init control pipe using raw syscalls.
     * PID 1 (init) periodically reads this file via check_initpipe(). */
    int fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)INITPIPE_PATH,
                               O_WRONLY | O_CREAT | O_TRUNC, 0, 0, 0);
    if (fd < 0) {
        kprintf("init: failed to open %s (err=%d)\n", INITPIPE_PATH, fd);
        return;
    }

    char content = (char)('0' + new_rl);
    long n = libc_syscall(SYS_WRITE, (uint64_t)(int64_t)fd,
                          (uint64_t)(uintptr_t)&content, 1, 0, 0);
    if (n < 0) {
        kprintf("init: write failed (err=%ld)\n", n);
    }

    libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);

    /* Send SIGHUP to PID 1 as a wake-up hint.  Even if init ignores
     * SIGHUP (the kernel blocks custom user signal handlers), the signal
     * delivery may wake init from its pause loop, causing it to check
     * the control file sooner. */
    libc_kill(1, 1); /* SIGHUP = 1 */

    kprintf("init: runlevel %d request sent\n", new_rl);
}
