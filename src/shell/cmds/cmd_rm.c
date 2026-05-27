/* cmd_rm.c — rm command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_rm(const char *args) {
    if (!args) { kprintf("Usage: rm <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; path[1] = '\0'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }
    if (fs_delete(path) < 0)
        kprintf("Cannot remove: %s\n", path);
}
