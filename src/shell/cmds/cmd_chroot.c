#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_chroot(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: chroot <args>\n");
        return;
    }
    kprintf("chroot: %s\n", args);
}
