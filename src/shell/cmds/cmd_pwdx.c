/* cmd_pwdx.c — print working directory of process */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_pwdx(void) {
    kprintf("/\n");
}
