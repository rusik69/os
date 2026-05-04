/* cmd_history.c — history command */
#include "shell_cmds.h"
#include "shell.h"

void cmd_history_show(void) {
    shell_history_show_entries();
}
