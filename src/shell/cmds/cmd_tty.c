/* cmd_tty.c — print terminal name */
#include "shell_cmds.h"
#include "printf.h"

void cmd_tty(void) {
    kprintf("/dev/console\n");
}
