#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_zipnote(const char *args) {
    if (!args) { kprintf("Usage: zipnote <file>\n"); return; }
    kprintf("zipnote: editing comment of '%s'\n", args);
}
