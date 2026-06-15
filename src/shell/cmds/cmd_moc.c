#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_moc(const char *args) {
    if (!args) { kprintf("Usage: moc <file>\n"); return; }
    kprintf("moc: playing '%s'\n", args);
}
