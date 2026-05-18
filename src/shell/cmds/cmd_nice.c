/* cmd_nice.c — set process scheduling priority */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_nice(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: nice <priority> <cmd> [args]\n");
        kprintf("  priority: 0=high 1=normal 2=low 3=idle\n");
        return;
    }
    int pri = (int)(args[0] - '0');
    if (pri < 0 || pri > 3) {
        kprintf("nice: priority must be 0-3\n");
        return;
    }
    /* Execute remaining command if provided */
    const char *rest = args + 1;
    while (*rest == ' ') rest++;
    int old_pri = -1;
    if (*rest) {
        old_pri = libc_getpriority((uint32_t)libc_getpid());
    }
    libc_setpriority(pri);
    if (*rest) {
        libc_shell_exec_cmd(rest, NULL);
        if (old_pri >= 0) libc_setpriority(old_pri);
    }
}
