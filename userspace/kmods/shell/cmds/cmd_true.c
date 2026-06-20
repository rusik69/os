/* cmd_true.c — always succeeds (exit 0) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_true(void) {
    shell_set_exit_status(0);
}
