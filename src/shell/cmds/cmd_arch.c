/* cmd_arch.c — print machine architecture */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_arch(const char *args) {
    (void)args;
    kprintf("x86_64\n");
}
