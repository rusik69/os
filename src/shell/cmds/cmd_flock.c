#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_flock(const char *args) {
    if (!args) { kprintf("Usage: flock <file> <command>\n"); return; }
    kprintf("flock: locking '%s'\n", args);
}
