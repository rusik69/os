#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_basenc(const char *args) {
    if (!args) { kprintf("Usage: basenc <file> [--decode] [--base64]\n"); return; }
    kprintf("basenc: reading '%s'\n", args);
}
