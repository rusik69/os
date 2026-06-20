#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_tset(const char *args) {
    if (!args) { kprintf("Usage: tset <type>\n"); return; }
    kprintf("tset: setting terminal type to '%s'\n", args);
}
