#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_runcon(const char *args) {
    if (!args) { kprintf("Usage: runcon <context> <command>\n"); return; }
    kprintf("runcon: running with context '%s'\n", args);
}
