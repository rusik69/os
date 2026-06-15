#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lesskey(const char *args) {
    if (!args) { kprintf("Usage: lesskey <file>\n"); return; }
    kprintf("lesskey: reading key bindings from '%s'\n", args);
}
