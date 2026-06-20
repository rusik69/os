/* cmd_unlink.c — unlink/delete file by path */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_unlink(const char *args) {
    if (!args || !*args) { kprintf("Usage: unlink <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); path[63] = '\0'; }
    else strncpy(path, args, 63);
    path[63] = '\0';

    if (libc_fs_delete(path) < 0) {
        kprintf("unlink: cannot unlink '%s'\n", args);
        shell_set_exit_status(1);
        return;
    }
    shell_set_exit_status(0);
}
