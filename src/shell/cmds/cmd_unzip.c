/* cmd_unzip.c — list/extract zip files (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_unzip(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unzip <file.zip> [files...]\n");
        return;
    }
    kprintf("unzip: not implemented\n");
}
