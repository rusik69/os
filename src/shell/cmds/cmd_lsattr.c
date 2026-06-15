#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lsattr(const char *args) {
    if (!args) { kprintf("Usage: lsattr <file>\n"); return; }
    kprintf("---------- %s\n", args);
}
