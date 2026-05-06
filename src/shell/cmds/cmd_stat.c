/* cmd_stat.c — stat command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_stat_file(const char *args) {
    if (!args) { kprintf("Usage: stat <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);

    uint32_t size; uint8_t type;
    uint16_t uid, gid, mode;
    if (fs_stat_ex(path, &size, &type, &uid, &gid, &mode) < 0) {
        kprintf("Not found: %s\n", path);
        return;
    }
    char mstr[10];
    fs_mode_str(mode, mstr);
    char type_char = (type == FS_TYPE_DIR) ? 'd' : '-';
    kprintf("  Path: %s\n", path);
    kprintf("  Type: %s\n", type == FS_TYPE_DIR ? "directory" : "file");
    kprintf("  Size: %u bytes\n", (uint64_t)size);
    kprintf("  Mode: %c%s (%u)\n", type_char, mstr, (uint64_t)mode);
    kprintf("  UID:  %u\n", (uint64_t)uid);
    kprintf("  GID:  %u\n", (uint64_t)gid);
}
