/* cmd_dmesg.c — dmesg command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_dmesg(void) {
    static char buf[16384];
    kprintf_dmesg(buf, sizeof(buf));
    kprintf("%s", buf);
}
