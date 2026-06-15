#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ipcrm(const char *args) {
    if (!args) { kprintf("Usage: ipcrm <id>\n"); return; }
    kprintf("ipcrm: removing %s\n", args);
}
