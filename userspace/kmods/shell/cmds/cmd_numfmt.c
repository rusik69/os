#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_numfmt(const char *args) {
    if (!args) { kprintf("Usage: numfmt <number>\n"); return; }
    kprintf("%s\n", args);
}
