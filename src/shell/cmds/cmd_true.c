/* cmd_true.c — always succeeds (exit 0); cmd_false — always fails */
#include "shell_cmds.h"
#include "printf.h"

void cmd_true(const char *args) {
    (void)args;
    /* success — no output */
}

void cmd_false(const char *args) {
    (void)args;
    /* failure — in a real shell this sets $? to 1 */
    kprintf("false\n");
}
