/* cmd_echo.c — echo command */
#include "shell_cmds.h"
#include "printf.h"

void cmd_echo(const char *args) {
    if (args) kprintf("%s\n", args);
    else kprintf("\n");
}
