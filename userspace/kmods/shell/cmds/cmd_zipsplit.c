#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_zipsplit(const char *args) {
    if (!args) { kprintf("Usage: zipsplit <file>\n"); return; }
    kprintf("zipsplit: splitting '%s'\n", args);
}
