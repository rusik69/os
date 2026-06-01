/* cmd_lzma.c — compress using LZMA (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_lzma(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: lzma <file>\n");
        return;
    }
    kprintf("lzma: not implemented\n");
}
