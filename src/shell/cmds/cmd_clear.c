/* cmd_clear.c — clear screen command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_clear(const char *args) {
    (void)args;
    libc_vga_clear();
}
