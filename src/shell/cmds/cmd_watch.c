/* cmd_watch.c — watch [-n secs] <command> */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_watch(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: watch [-n secs] <cmd>\n");
        shell_set_exit_status(1);
        return;
    }

    int interval = 2; /* default 2 seconds */
    int i = 0;
    while (args[i] == ' ') i++;
    if (args[i] == '-' && args[i+1] == 'n') {
        i += 2;
        while (args[i] == ' ') i++;
        interval = 0;
        while (args[i] >= '0' && args[i] <= '9') { interval = interval*10 + (args[i]-'0'); i++; }
        while (args[i] == ' ') i++;
    }
    if (!args[i]) { kprintf("watch: no command\n"); shell_set_exit_status(1); return; }

    const char *cmd = args + i;
    /* Repeat ~10 times (no interactive quit in kernel shell) */
    for (int iter = 0; iter < 10; iter++) {
        kprintf("\033[2J\033[H");  /* ANSI clear — VGA ignores, telnet respects */
        kprintf("Every %ds: %s\n\n", interval, cmd);
        libc_shell_exec_cmd(cmd, NULL);
        libc_sleep_ticks((uint64_t)(interval * 100)); /* 100 ticks/sec */
    }
}
