/* cmd_true.c — always succeeds (exit 0); cmd_false — always fails */
#include "shell_cmds.h"
#include "printf.h"

void cmd_true(const char *args) {
    (void)args;
    shell_set_exit_status(0);
}

void cmd_false(const char *args) {
    (void)args;
    shell_set_exit_status(1);
}
