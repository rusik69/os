/* cmd_rm.c — rm command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "ata.h"

void cmd_rm(const char *args) {
    if (!args) { kprintf("Usage: rm <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    if (fs_delete(path) < 0)
        kprintf("Cannot remove: %s\n", path);
}
