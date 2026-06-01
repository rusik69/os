#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_chrt(const char *args) {
    if (!args) { kprintf("Usage: chrt [policy] <pid>\n"); return; }
    kprintf("chrt: setting scheduler policy (simulated)\n");
}
