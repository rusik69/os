#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha384sum(const char *args) {
    if (!args) { kprintf("Usage: sha384sum <file>\n"); return; }
    kprintf("sha384sum: computing hash of '%s'\n", args);
}
