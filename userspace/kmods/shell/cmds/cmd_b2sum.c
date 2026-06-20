#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_b2sum(const char *args) {
    if (!args) { kprintf("Usage: b2sum <file>\n"); return; }
    kprintf("b2sum: reading '%s'\n", args);
}
