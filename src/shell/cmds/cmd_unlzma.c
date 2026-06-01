/* cmd_unlzma.c — decompress .lzma files (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_unlzma(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unlzma <file.lzma>\n");
        return;
    }
    kprintf("unlzma: not implemented\n");
}
