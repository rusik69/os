#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha224sum(const char *args) {
    if (!args) { kprintf("Usage: sha224sum <file>\n"); return; }
    kprintf("sha224sum: computing hash of '%s'\n", args);
}
