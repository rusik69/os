/* cmd_xz.c — compress using LZMA2 (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_xz(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: xz <file>\n");
        return;
    }
    kprintf("xz: not implemented\n");
}
