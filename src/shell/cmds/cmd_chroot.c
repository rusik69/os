#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_chroot(const char *args) {
    if (!args) { kprintf("Usage: chroot <dir> [command]\n"); return; }
    kprintf("chroot: changing root to '%s'\n", args);
}
