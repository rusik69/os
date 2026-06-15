#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha512sum(const char *args) {
    if (!args) { kprintf("Usage: sha512sum <file>\n"); return; }
    kprintf("sha512sum: computing hash of '%s'\n", args);
}
