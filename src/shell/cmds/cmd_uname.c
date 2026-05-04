/* cmd_uname.c — uname command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_uname(void) {
    kprintf("OS kernel x86_64 v0.1\n");
}
