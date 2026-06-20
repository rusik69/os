#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lessecho(const char *args) {
    if (!args) { kprintf("\n"); return; }
    kprintf("%s\n", args);
}
