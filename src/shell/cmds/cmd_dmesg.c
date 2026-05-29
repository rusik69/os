/* cmd_dmesg.c — dmesg command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_dmesg(const char *args) {
    /* Skip leading spaces */
    while (args && *args == ' ') args++;
    int do_clear = (args && args[0] == '-' && args[1] == 'c');

    static char buf[16384];
    int n = kprintf_dmesg(buf, sizeof(buf));
    kprintf("%s", buf);
    (void)n;

    if (do_clear)
        kprintf_dmesg_clear();
}
