/* cmd_history.c — history command */
#include "shell_cmds.h"
#include "libc.h"

void cmd_history_show(void) {
    libc_shell_history_show();
}
