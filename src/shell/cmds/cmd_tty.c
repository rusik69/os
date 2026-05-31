/* cmd_tty.c — print terminal device name */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_tty(const char *args) {
    (void)args;
    /* In this kernel shell we always run on the VGA console */
    kprintf("/dev/console\n");
}
