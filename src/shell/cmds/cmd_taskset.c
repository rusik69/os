/* cmd_taskset.c — get/set CPU affinity for processes */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "syscall.h"

void cmd_taskset(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: taskset [-p <pid>] <mask>\n");
        kprintf("  mask: CPU bitmask (e.g., 0x01 for CPU 0)\n");
        return;
    }

    uint32_t pid = libc_getpid();
    const char *p = args;

    /* Check for -p flag */
    if (p[0] == '-' && p[1] == 'p') {
        p += 2;
        while (*p == ' ') p++;
        char pidstr[16];
        int i = 0;
        while (*p && *p != ' ' && i < 15) pidstr[i++] = *p++;
        pidstr[i] = '\0';
        pid = (uint32_t)strtol(pidstr, 0, 10);
        while (*p == ' ') p++;
    }

    if (!*p) {
        /* Query current affinity */
        uint64_t mask = libc_syscall(SYS_SCHED_GETAFFINITY, pid, 0, 0, 0, 0);
        if (mask == (uint64_t)-1) {
            kprintf("taskset: failed to get affinity for pid %u\n", (uint64_t)pid);
            shell_set_exit_status(1);
            return;
        }
        kprintf("pid %u: current affinity mask 0x%x\n", (uint64_t)pid, (uint64_t)mask);
        return;
    }

    /* Parse mask */
    char maskstr[16];
    int i = 0;
    while (*p && *p != ' ' && i < 15) maskstr[i++] = *p++;
    maskstr[i] = '\0';

    uint64_t mask = strtoul(maskstr, 0, 16); /* assume hex */

    uint64_t ret = libc_syscall(SYS_SCHED_SETAFFINITY, pid, mask, 0, 0, 0);
    if (ret == (uint64_t)-1) {
        kprintf("taskset: failed to set affinity for pid %u\n", (uint64_t)pid);
        shell_set_exit_status(1);
        return;
    }
    kprintf("taskset: pid %u affinity set to 0x%x\n", (uint64_t)pid, (uint64_t)mask);
    shell_set_exit_status(0);
}
