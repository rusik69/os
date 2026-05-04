/* cmd_hostname.c — hostname command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_hostname(void) {
    kprintf("os-kernel\n");
}
