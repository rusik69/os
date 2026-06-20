#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_setsid(const char *args) {
    if (!args) { kprintf("Usage: setsid <command>\n"); return; }
    kprintf("setsid: running '%s' in new session\n", args);
}
