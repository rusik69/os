#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_csplit(const char *args) {
    if (!args) { kprintf("Usage: csplit <file> <pattern>\n"); return; }
    kprintf("csplit: splitting '%s'\n", args);
}
