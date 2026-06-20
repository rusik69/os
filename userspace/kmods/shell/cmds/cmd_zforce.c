#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_zforce(const char *args) {
    if (!args) { kprintf("Usage: zforce <file>\n"); return; }
    kprintf("zforce: renaming '%s'\n", args);
}
