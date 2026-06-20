/* cmd_false.c — always exit 1 */
#include "shell_cmds.h"
#include "printf.h"

void cmd_false(void) {
    shell_set_exit_status(1);
}
