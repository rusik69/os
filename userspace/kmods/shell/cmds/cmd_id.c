/* cmd_id.c — Print user/group identity
 *
 * The shell runs as a user-space process (/mnt/bin/sh), so it MUST obtain
 * identity via value-returning syscalls.  The kernel's session struct pointer
 * (returned by libc_session_get / session_get) lives in kernel address space
 * and is NOT dereferenceable from ring-3 — doing so page-faults the shell and
 * drops the telnet session, which is why earlier implementations crashed.
 * We use libc_syscall(SYS_GETUID/SYS_GETGID), which are safe in both the
 * user-space shell and the kernel module build.
 */

#include "shell_cmds.h"
#include "libc.h"
#include "syscall.h"
#include "printf.h"

void cmd_id(const char *args) {
    (void)args;
    uint32_t uid = (uint32_t)libc_syscall(SYS_GETUID, 0, 0, 0, 0, 0);
    uint32_t gid = (uint32_t)libc_syscall(SYS_GETGID, 0, 0, 0, 0, 0);

    /* At boot / as root the identity is literally root.  Non-root sessions
     * would normally resolve a name from the user database, but the session
     * pointer is not usable from user space; print the numeric id with a
     * sensible placeholder name. */
    const char *name = (uid == 0) ? "root" : "user";
    kprintf("uid=%u(%s) gid=%u(%s)\n", uid, name, gid, name);
}
