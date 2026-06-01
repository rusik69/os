#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_echo(const char *args) {
    if (!args) { kprintf("\n"); return; }
    kprintf("%s\n", args);
}
