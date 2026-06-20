#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_clear(const char *args) {
    (void)args;
    kprintf("\033[2J\033[H");
}
