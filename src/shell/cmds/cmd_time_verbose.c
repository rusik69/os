#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_time_verbose(const char *args) {
    if (!args) { kprintf("Usage: time_verbose <command>\n"); return; }
    kprintf("time_verbose: timing '%s'\n", args);
}
