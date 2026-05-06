/* cmd_ls.c — ls command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ls(const char *args) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    if (fs_list(args ? args : "/") < 0)
        kprintf("Not a directory or not found\n");
}
