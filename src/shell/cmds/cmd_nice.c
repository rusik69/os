/* cmd_nice.c — run a command with modified scheduling priority (nice value) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "syscall.h"

void cmd_nice(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: nice <nice_value> <cmd> [args]\n");
        kprintf("  nice_value: -20 (highest priority) to +19 (lowest priority)\n");
        return;
    }

    /* Parse nice value (may be negative) */
    int sign = 1;
    const char *p = args;
    while (*p == ' ') p++;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    int nice = 0;
    while (*p >= '0' && *p <= '9') {
        nice = nice * 10 + (*p - '0');
        p++;
    }
    nice *= sign;

    /* Clamp to valid range */
    if (nice < NICE_MIN) nice = NICE_MIN;
    if (nice > NICE_MAX) nice = NICE_MAX;

    /* Skip past the nice value */
    while (*p == ' ') p++;

    int old_nice = -1;
    if (*p) {
        /* There's a command to run — save old nice value for restoration */
        old_nice = libc_getpriority(PRIO_PROCESS, 0);
    }

    /* Set new nice value */
    libc_setpriority(PRIO_PROCESS, 0, nice);

    if (*p) {
        /* Execute the command, then restore original priority */
        libc_shell_exec_cmd(p, NULL);
        if (old_nice >= NICE_MIN && old_nice <= NICE_MAX)
            libc_setpriority(PRIO_PROCESS, 0, old_nice);
    }
}
