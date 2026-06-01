/* cmd_unxz.c — decompress .xz files (stub) */
#include "shell_cmds.h"
#include "printf.h"

void cmd_unxz(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: unxz <file.xz>\n");
        return;
    }
    kprintf("unxz: not implemented\n");
}
