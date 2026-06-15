/* cmd_capsh.c — capability shell: list current process capabilities */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "process.h"
#include "caps.h"

/* Capability names indexed by number */
static const char *cap_name(int cap)
{
    static const char *names[] = {
        [0]  = "CAP_CHOWN",
        [1]  = "CAP_DAC_OVERRIDE",
        [2]  = "CAP_DAC_READ_SEARCH",
        [3]  = "CAP_FOWNER",
        [4]  = "CAP_FSETID",
        [5]  = "CAP_KILL",
        [6]  = "CAP_SETGID",
        [7]  = "CAP_SETUID",
        [8]  = "CAP_SETPCAP",
        [10] = "CAP_NET_BIND_SERVICE",
        [11] = "CAP_NET_BROADCAST",
        [12] = "CAP_NET_ADMIN",
        [13] = "CAP_NET_RAW",
        [14] = "CAP_IPC_LOCK",
        [15] = "CAP_IPC_OWNER",
        [16] = "CAP_SYS_MODULE",
        [17] = "CAP_SYS_RAWIO",
        [18] = "CAP_SYS_CHROOT",
        [19] = "CAP_SYS_PTRACE",
        [20] = "CAP_SYS_PACCT",
        [21] = "CAP_SYS_ADMIN",
        [22] = "CAP_SYS_BOOT",
        [23] = "CAP_SYS_NICE",
        [24] = "CAP_SYS_RESOURCE",
        [25] = "CAP_SYS_TIME",
        [26] = "CAP_SYS_TTY_CONFIG",
        [27] = "CAP_MKNOD",
        [28] = "CAP_LEASE",
        [29] = "CAP_AUDIT_WRITE",
        [30] = "CAP_AUDIT_CONTROL",
        [31] = "CAP_SETFCAP",
        [32] = "CAP_MAC_OVERRIDE",
        [33] = "CAP_MAC_ADMIN",
        [34] = "CAP_SYSLOG",
        [35] = "CAP_WAKE_ALARM",
        [36] = "CAP_BLOCK_SUSPEND",
        [37] = "CAP_AUDIT_READ",
        [38] = "CAP_NET_ADMIN_RAW",
    };
    if (cap >= 0 && cap <= 38 && names[cap])
        return names[cap];
    return "CAP_UNKNOWN";
}

int cmd_capsh(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    struct process *proc = process_get_current();
    if (!proc) {
        kprintf("capsh: could not get current process\n");
        return 1;
    }

    kprintf("capability state for PID %u (%s):\n",
            (unsigned int)proc->pid, proc->name ? proc->name : "unnamed");
    kprintf("  cap_profile: ");
    switch (proc->cap_profile) {
        case PROCESS_CAP_PROFILE_NONE:
            kprintf("none\n");
            break;
        case PROCESS_CAP_PROFILE_USER_DEFAULT:
            kprintf("user-default\n");
            break;
        case PROCESS_CAP_PROFILE_USER_TRUSTED:
            kprintf("user-trusted\n");
            break;
        default:
            kprintf("%u\n", (unsigned int)proc->cap_profile);
            break;
    }

    kprintf("  Effective capabilities:\n");
    int found = 0;
    for (int i = 0; i <= 38; i++) {
        int word = i / 64;
        int bit  = i % 64;
        if (word < PROCESS_SYSCALL_CAP_WORDS &&
            (proc->cap_effective[word] & (1ULL << bit))) {
            kprintf("    %s\n", cap_name(i));
            found = 1;
        }
    }
    if (!found)
        kprintf("    (none)\n");

    kprintf("  Permitted capabilities:\n");
    found = 0;
    for (int i = 0; i <= 38; i++) {
        int word = i / 64;
        int bit  = i % 64;
        if (word < PROCESS_SYSCALL_CAP_WORDS &&
            (proc->cap_permitted[word] & (1ULL << bit))) {
            kprintf("    %s\n", cap_name(i));
            found = 1;
        }
    }
    if (!found)
        kprintf("    (none)\n");

    return 0;
}

void capsh_init(void)
{
    kprintf("[OK] cmd_capsh: capability shell ready\n");
}
