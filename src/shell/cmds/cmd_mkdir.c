/* cmd_mkdir.c — mkdir command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_mkdir(const char *args) {
    if (!args) { kprintf("Usage: mkdir <dir>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    if (fs_create(path, FS_TYPE_DIR) < 0)
        kprintf("Cannot create directory: %s\n", path);
}
