#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_pax(const char *args) {
    if (!args) { kprintf("Usage: pax -w <file>\n"); return; }
    kprintf("pax: archiving '%s'\n", args);
}
