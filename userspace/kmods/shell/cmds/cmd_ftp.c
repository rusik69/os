#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ftp(const char *args) {
    if (!args) { kprintf("Usage: ftp <hostname>\n"); return; }
    kprintf("ftp: connecting to '%s'\n", args);
}
