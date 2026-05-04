/* cmd_stat.c — stat command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "ata.h"

void cmd_stat_file(const char *args) {
    if (!args) { kprintf("Usage: stat <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) {
        kprintf("Not found: %s\n", path);
        return;
    }
    kprintf("  Path: %s\n", path);
    kprintf("  Type: %s\n", type == FS_TYPE_DIR ? "directory" : "file");
    kprintf("  Size: %u bytes\n", (uint64_t)size);
}
