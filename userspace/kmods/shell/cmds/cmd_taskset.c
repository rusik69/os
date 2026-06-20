#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_taskset(const char *args) {
    if (!args) { kprintf("Usage: taskset <mask> <command>\n"); return; }
    kprintf("taskset: set affinity mask %s\n", args);
}
