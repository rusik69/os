#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha256sum(const char *args) {
    if (!args) { kprintf("Usage: sha256sum <file>\n"); return; }
    kprintf("sha256sum: computing hash of '%s'\n", args);
}
