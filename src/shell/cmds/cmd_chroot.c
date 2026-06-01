/* cmd_chroot.c — change root directory */
#include "shell_cmds.h"
#include "printf.h"

void cmd_chroot(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: chroot <newroot> [command]\n");
        return;
    }
    kprintf("chroot: not implemented\n");
}
