#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
void cmd_colrm(const char *args) {
    if (!args) { kprintf("Usage: colrm <start> [end]\n"); return; }
    kprintf("colrm: remove columns %s\n", args);
}
