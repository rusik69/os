#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_zipcloak(const char *args) {
    if (!args) { kprintf("Usage: zipcloak <file>\n"); return; }
    kprintf("zipcloak: encrypting '%s'\n", args);
}
