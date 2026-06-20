#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_install(const char *args) {
    if (!args) { kprintf("Usage: install <src> <dst>\n"); return; }
    kprintf("install: copying '%s'\n", args);
}
