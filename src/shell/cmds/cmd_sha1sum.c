#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha1sum(const char *args) {
    if (!args) { kprintf("Usage: sha1sum <file>\n"); return; }
    kprintf("sha1sum: computing hash of '%s'\n", args);
}
