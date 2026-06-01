#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_taskset(const char *args) {
    if (!args) { kprintf("Usage: taskset <mask> <pid>\n"); return; }
    kprintf("taskset: setting CPU affinity (simulated)\n");
}
