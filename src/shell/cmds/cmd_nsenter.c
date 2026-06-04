/*
 * cmd_nsenter.c — nsenter: join an existing process's namespace (Item 120)
 *
 * Usage:
 *   nsenter --target <pid> --uts    [--pid] [--mnt] [--net] [--ipc]
 *           [--cgroup] [--time] [--program <cmd> [args...]]
 *
 * Opens /proc/<pid>/ns/<type> and calls setns(fd, nstype) to join the
 * target process's namespace.  Requires CAP_SYS_ADMIN (root).
 *
 * Examples:
 *   nsenter -t 42 -u                     ← join UTS ns of PID 42
 *   nsenter --target 42 --uts --pid      ← join UTS + PID ns
 *   nsenter -t 42 -u -p -n /bin/sh       ← join UTS+PID+NET, run shell
 */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

/* Namespace clone flags (Linux-compatible, duplicated from kernel process.h) */
#define CLONE_NEWNS      0x00020000
#define CLONE_NEWUTS     0x04000000
#define CLONE_NEWPID     0x20000000
#define CLONE_NEWNET     0x40000000
#define CLONE_NEWIPC     0x08000000
#define CLONE_NEWCGROUP  0x02000000
#define CLONE_NEWTIME    0x00000080

/* Mapping from namespace type name to its CLONE_NEW* flag */
struct ns_entry {
    const char *name;
    uint64_t    flag;
};

static const struct ns_entry ns_types[] = {
    { "uts",    CLONE_NEWUTS    },
    { "pid",    CLONE_NEWPID    },
    { "mnt",    CLONE_NEWNS     },
    { "net",    CLONE_NEWNET    },
    { "ipc",    CLONE_NEWIPC    },
    { "cgroup", CLONE_NEWCGROUP },
    { "time",   CLONE_NEWTIME   },
};
#define NS_TYPE_COUNT  (sizeof(ns_types) / sizeof(ns_types[0]))

/* ── Help text ────────────────────────────────────────────────────────── */

static void nsenter_help(void) {
    kprintf("Usage: nsenter --target <pid> [namespace-opts] [--program <cmd>]\n");
    kprintf("Join the namespace(s) of another process.\n");
    kprintf("  -t, --target <pid>   PID of target process\n");
    kprintf("  -u, --uts            join UTS namespace (hostname)\n");
    kprintf("  -p, --pid            join PID namespace\n");
    kprintf("  -m, --mnt            join mount namespace\n");
    kprintf("  -n, --net            join network namespace\n");
    kprintf("  -i, --ipc            join IPC namespace\n");
    kprintf("  -c, --cgroup         join cgroup namespace\n");
    kprintf("  -e, --time           join time namespace\n");
    kprintf("  --program <cmd>      run command after joining\n");
}

/* ── Main entry ───────────────────────────────────────────────────────── */

void cmd_nsenter(const char *args) {
    if (!args || *args == '\0') {
        nsenter_help();
        return;
    }

    uint32_t target_pid = 0;
    uint64_t ns_mask = 0;
    char cmd_buf[128];
    cmd_buf[0] = '\0';
    const char *p = args;

    /* Simple space-delimited argument parser */
    while (p && *p) {
        /* Skip leading spaces */
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        /* Extract next token */
        char tok[64];
        int ti = 0;
        while (*p && *p != ' ' && *p != '\t' && ti < 63)
            tok[ti++] = *p++;
        tok[ti] = '\0';

        if (strcmp(tok, "--target") == 0 || strcmp(tok, "-t") == 0) {
            /* Skip spaces after flag */
            while (*p == ' ' || *p == '\t') p++;
            /* Read PID */
            char pid_str[16];
            int pi = 0;
            while (*p && *p >= '0' && *p <= '9' && pi < 15)
                pid_str[pi++] = *p++;
            pid_str[pi] = '\0';
            if (pi == 0) {
                kprintf("nsenter: --target requires a PID\n");
                return;
            }
            target_pid = 0;
            for (int i = 0; pid_str[i]; i++)
                target_pid = target_pid * 10 + (uint32_t)(pid_str[i] - '0');
        } else if (strcmp(tok, "--uts") == 0 || strcmp(tok, "-u") == 0) {
            ns_mask |= CLONE_NEWUTS;
        } else if (strcmp(tok, "--pid") == 0 || strcmp(tok, "-p") == 0) {
            ns_mask |= CLONE_NEWPID;
        } else if (strcmp(tok, "--mnt") == 0 || strcmp(tok, "-m") == 0) {
            ns_mask |= CLONE_NEWNS;
        } else if (strcmp(tok, "--net") == 0 || strcmp(tok, "-n") == 0) {
            ns_mask |= CLONE_NEWNET;
        } else if (strcmp(tok, "--ipc") == 0 || strcmp(tok, "-i") == 0) {
            ns_mask |= CLONE_NEWIPC;
        } else if (strcmp(tok, "--cgroup") == 0 || strcmp(tok, "-c") == 0) {
            ns_mask |= CLONE_NEWCGROUP;
        } else if (strcmp(tok, "--time") == 0 || strcmp(tok, "-e") == 0) {
            ns_mask |= CLONE_NEWTIME;
        } else if (strcmp(tok, "--program") == 0) {
            /* Everything remaining is the command */
            while (*p == ' ' || *p == '\t') p++;
            int ci = 0;
            while (*p && ci < (int)sizeof(cmd_buf) - 1)
                cmd_buf[ci++] = *p++;
            cmd_buf[ci] = '\0';
            break;
        } else if (strcmp(tok, "--help") == 0 || strcmp(tok, "-h") == 0) {
            nsenter_help();
            return;
        } else {
            kprintf("nsenter: unknown option '%s'\n", tok);
            return;
        }
    }

    /* Validate arguments */
    if (target_pid == 0) {
        kprintf("nsenter: --target <pid> is required\n");
        return;
    }
    if (ns_mask == 0) {
        kprintf("nsenter: at least one namespace type required (e.g. --uts)\n");
        return;
    }

    /* For each requested namespace type, open /proc/<pid>/ns/<type>
     * and call setns(fd, nstype) via the SYS_SETNS syscall. */
    for (size_t i = 0; i < NS_TYPE_COUNT; i++) {
        if (!(ns_mask & ns_types[i].flag))
            continue;

        /* Build path: /proc/<pid>/ns/<name> */
        char ns_path[64];
        int pos = 0;
        const char *prefix = "/proc/";
        while (*prefix) ns_path[pos++] = *prefix++;
        /* Append PID digits */
        {
            uint32_t pid = target_pid;
            char pid_rev[12];
            int ri = 0;
            if (pid == 0) { pid_rev[ri++] = '0'; }
            else { while (pid) { pid_rev[ri++] = '0' + (int)(pid % 10); pid /= 10; } }
            for (int j = ri - 1; j >= 0 && pos < (int)sizeof(ns_path) - 20; j--)
                ns_path[pos++] = pid_rev[j];
        }
        /* Append /ns/<type> */
        ns_path[pos++] = '/'; ns_path[pos++] = 'n'; ns_path[pos++] = 's'; ns_path[pos++] = '/';
        const char *t = ns_types[i].name;
        while (*t && pos < (int)sizeof(ns_path) - 1)
            ns_path[pos++] = *t++;
        ns_path[pos] = '\0';

        /* Open the namespace file via SYS_OPEN (O_RDONLY = 0) */
        int64_t fd = (int64_t)libc_syscall(SYS_OPEN,
            (uint64_t)(uintptr_t)ns_path, (uint64_t)0, 0, 0, 0);
        if (fd < 0) {
            kprintf("nsenter: cannot open %s\n", ns_path);
            return;
        }

        /* Call setns(fd, nstype) via SYS_SETNS */
        int64_t ret = (int64_t)libc_syscall(SYS_SETNS,
            (uint64_t)(uintptr_t)(size_t)fd,
            ns_types[i].flag, 0, 0, 0);
        if (ret < 0) {
            kprintf("nsenter: setns(%s) failed on PID %u (error %lld)\n",
                    ns_types[i].name, (unsigned int)target_pid,
                    (long long)-ret);
            /* Close the fd before returning */
            libc_syscall(SYS_CLOSE, (uint64_t)(uintptr_t)(size_t)fd, 0, 0, 0, 0);
            return;
        }

        /* Close the namespace fd */
        libc_syscall(SYS_CLOSE, (uint64_t)(uintptr_t)(size_t)fd, 0, 0, 0, 0);
    }

    /* If a command was specified, execute it in the new namespace context */
    if (cmd_buf[0]) {
        int exec_ret = libc_elf_exec(cmd_buf);
        if (exec_ret < 0)
            kprintf("nsenter: exec '%s' failed\n", cmd_buf);
    } else {
        kprintf("nsenter: joined namespace(s) of PID %u\n",
                (unsigned int)target_pid);
    }
}
