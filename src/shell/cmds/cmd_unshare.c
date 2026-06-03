/* cmd_unshare.c — unshare: disassociate parts of process context (Item 119)
 *
 * Usage: unshare [--mount] [--uts] [--pid] [--net] [--ipc] [--fork] [command]
 *   --mount    CLONE_NEWNS   — unshare mount namespace
 *   --uts      CLONE_NEWUTS  — unshare hostname/domainname namespace
 *   --pid      CLONE_NEWPID  — unshare PID namespace
 *   --net      CLONE_NEWNET  — unshare network namespace
 *   --ipc      CLONE_NEWIPC  — unshare IPC namespace
 *   --fork     -f            — fork before exec (like Linux unshare -f)
 *
 * If a command is given, it is executed with the new namespaces.
 * Without a command, the current process's namespaces are unshared.
 *
 * Example:
 *   unshare --uts /bin/hostname mycontainer
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/* Namespace clone flags (Linux-compatible, duplicated here since user-space
 * commands don't include kernel process.h). */
#define CLONE_NEWNS      0x00020000
#define CLONE_NEWUTS     0x04000000
#define CLONE_NEWPID     0x20000000
#define CLONE_NEWNET     0x40000000
#define CLONE_NEWIPC     0x08000000

/* Parse namespace flags from command-line arguments.
 * Returns the flags bitmask, with bit 32 set for --fork. */
static uint64_t parse_ns_flags(const char *args, const char **cmd_out)
{
    uint64_t flags = 0;
    const char *p = args;

    if (cmd_out)
        *cmd_out = NULL;

    while (p && *p) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (!*p) break;

        if (*p == '-') {
            p++; /* skip first dash */
            if (*p == '-') p++; /* skip second dash */

            if (strncmp(p, "mount", 5) == 0) {
                flags |= CLONE_NEWNS;
                p += 5;
            } else if (strncmp(p, "uts", 3) == 0) {
                flags |= CLONE_NEWUTS;
                p += 3;
            } else if (strncmp(p, "pid", 3) == 0) {
                flags |= CLONE_NEWPID;
                p += 3;
            } else if (strncmp(p, "net", 3) == 0) {
                flags |= CLONE_NEWNET;
                p += 3;
            } else if (strncmp(p, "ipc", 3) == 0) {
                flags |= CLONE_NEWIPC;
                p += 3;
            } else if (strncmp(p, "fork", 4) == 0 || (p[0] == 'f' && p[1] != 'o')) {
                flags |= (1ULL << 32); /* fork flag */
                p += (p[0] == 'f' && p[1] != 'o') ? 1 : 4;
            } else if (strncmp(p, "help", 4) == 0 || *p == 'h') {
                /* Skip unknown */
                while (*p && *p != ' ') p++;
            } else {
                /* Unknown flag - skip word */
                while (*p && *p != ' ') p++;
            }
        } else {
            /* First non-flag argument is the command (path) */
            if (cmd_out)
                *cmd_out = p;
            break;
        }
    }

    return flags;
}

void cmd_unshare(const char *args)
{
    if (!args || !*args) {
        kprintf("Usage: unshare [--mount] [--uts] [--pid] [--net] [--ipc] [--fork] [command [args...]]\n");
        kprintf("Disassociate parts of the process execution context.\n");
        kprintf("  --mount   unshare mount namespace\n");
        kprintf("  --uts     unshare hostname/domainname\n");
        kprintf("  --pid     unshare PID numbering\n");
        kprintf("  --net     unshare network stack\n");
        kprintf("  --ipc     unshare IPC resources\n");
        kprintf("  --fork    fork before exec'ing command\n");
        return;
    }

    const char *cmd = NULL;
    uint64_t raw_flags = parse_ns_flags(args, &cmd);
    uint64_t ns_flags = raw_flags & 0xFFF00000ULL; /* only namespace bits */
    int do_fork = (raw_flags >> 32) & 1;

    if (raw_flags == 0 && !cmd) {
        /* No flags and no command — show help */
        cmd_unshare("");
        return;
    }

    /* Call the unshare syscall via libc */
    int ret = libc_syscall(SYS_UNSHARE, ns_flags, 0, 0, 0, 0);
    if ((int64_t)ret < 0) {
        kprintf("unshare: failed (errno=%lld)\n", (long long)(int64_t)ret);
        return;
    }

    /* If a command was given, execute it */
    if (cmd && *cmd) {
        if (do_fork) {
            /* Fork first, then exec in child */
            int pid = libc_fork();
            if (pid < 0) {
                kprintf("unshare: fork failed\n");
                return;
            }
            if (pid == 0) {
                /* Child: exec the command */
                libc_elf_exec(cmd);
                /* exec failed — shouldn't return */
                kprintf("unshare: exec '%s' failed\n", cmd);
            }
            /* Parent: wait for child */
            int status;
            libc_waitpid(pid, &status);
        } else {
            /* Directly exec into the command */
            libc_elf_exec(cmd);
            kprintf("unshare: exec '%s' failed\n", cmd);
        }
    }
}
