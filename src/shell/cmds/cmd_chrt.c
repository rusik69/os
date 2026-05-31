/* cmd_chrt.c — get/set real-time scheduling attributes */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "syscall.h"

void cmd_chrt(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: chrt [-p <pid>] <policy> [priority]\n");
        kprintf("  Policies: other(0), fifo(1), rr(2)\n");
        return;
    }

    int is_set = 0;
    uint32_t pid = libc_getpid();
    int policy = -1;
    int priority = 0;

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
        /* Query: show current scheduling policy */
        uint64_t cur_policy = libc_syscall(SYS_SCHED_GETSCHEDULER, pid, 0, 0, 0, 0);
        const char *pname;
        if (cur_policy == (uint64_t)-1) {
            kprintf("chrt: cannot get scheduler for pid %u\n", (uint64_t)pid);
            shell_set_exit_status(1);
            return;
        }
        if (cur_policy == SCHED_OTHER) pname = "SCHED_OTHER";
        else if (cur_policy == SCHED_FIFO) pname = "SCHED_FIFO";
        else if (cur_policy == SCHED_RR) pname = "SCHED_RR";
        else pname = "UNKNOWN";
        kprintf("pid %u: policy %s (%u)\n", (uint64_t)pid, pname, (uint64_t)cur_policy);
        return;
    }

    /* Parse policy name/number */
    char polstr[16];
    int i = 0;
    while (*p && *p != ' ' && i < 15) polstr[i++] = *p++;
    polstr[i] = '\0';

    if (strcmp(polstr, "other") == 0 || strcmp(polstr, "0") == 0) policy = SCHED_OTHER;
    else if (strcmp(polstr, "fifo") == 0 || strcmp(polstr, "1") == 0) policy = SCHED_FIFO;
    else if (strcmp(polstr, "rr") == 0 || strcmp(polstr, "2") == 0) policy = SCHED_RR;
    else {
        kprintf("chrt: unknown policy '%s'\n", polstr);
        shell_set_exit_status(1);
        return;
    }

    while (*p == ' ') p++;
    if (*p) {
        char prstr[16];
        i = 0;
        while (*p && *p != ' ' && i < 15) prstr[i++] = *p++;
        prstr[i] = '\0';
        priority = (int)strtol(prstr, 0, 10);
    }

    struct sched_param param;
    param.sched_priority = priority;

    uint64_t ret = libc_syscall(SYS_SCHED_SETSCHEDULER, pid, policy,
                                 (uint64_t)(uintptr_t)&param, 0, 0);
    if (ret == (uint64_t)-1) {
        kprintf("chrt: failed to set scheduler for pid %u\n", (uint64_t)pid);
        shell_set_exit_status(1);
        return;
    }
    kprintf("chrt: pid %u set to policy %d, priority %d\n",
            (uint64_t)pid, policy, priority);
    shell_set_exit_status(0);
}
