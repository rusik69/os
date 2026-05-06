/* cmd_time.c — time: measure execution time of a shell command */
#include "shell_cmds.h"
#include "shell.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_time(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: time <command> [args...]\n");
        return;
    }

    /* Split into command name + rest */
    char cmdbuf[256];
    strncpy(cmdbuf, args, 255);
    cmdbuf[255] = '\0';

    char *cmdname = cmdbuf;
    char *cmdargs = cmdbuf;
    while (*cmdargs && *cmdargs != ' ') cmdargs++;
    if (*cmdargs) {
        *cmdargs = '\0';
        cmdargs++;
        while (*cmdargs == ' ') cmdargs++;
    } else {
        cmdargs = NULL;
    }

    uint64_t start = libc_uptime_ticks();
    shell_exec_cmd(cmdname, cmdargs);
    uint64_t end = libc_uptime_ticks();

    uint64_t elapsed_ms = (end - start) * 1000 / TIMER_FREQ;
    uint64_t sec = elapsed_ms / 1000;
    uint64_t ms  = elapsed_ms % 1000;

    /* Print with 3-digit millisecond padding (kprintf has no field-width) */
    kprintf("\nreal\t%u.", sec);
    if (ms < 100) kprintf("0");
    if (ms < 10)  kprintf("0");
    kprintf("%us\n", ms);
}
