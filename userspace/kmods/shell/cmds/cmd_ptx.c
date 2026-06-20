#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ptx(const char *args) {
    if (!args) { kprintf("Usage: ptx <file>\n"); return; }
    kprintf("ptx: reading '%s'\n", args);
}
