/* cmd_rmdir.c — remove empty directory */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_rmdir(const char *args) {
    if (!args || !*args) { kprintf("Usage: rmdir <directory>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); path[63] = '\0'; }
    else strncpy(path, args, 63);
    path[63] = '\0';

    uint32_t size;
    uint8_t type;
    if (fs_stat(path, &size, &type) < 0) {
        kprintf("rmdir: cannot stat '%s'\n", path);
        shell_set_exit_status(1);
        return;
    }
    if (type != FS_TYPE_DIR) {
        kprintf("rmdir: '%s' is not a directory\n", path);
        shell_set_exit_status(1);
        return;
    }

    /* Try to list directory; if it has entries besides . and .., refuse */
    char names[16][FS_MAX_NAME];
    int n = fs_list_names(path, "", names, 16);
    if (n < 0) {
        kprintf("rmdir: cannot read directory '%s'\n", path);
        shell_set_exit_status(1);
        return;
    }
    /* Count non-empty entries */
    int count = 0;
    for (int i = 0; i < n; i++) {
        if (names[i][0] && strcmp(names[i], ".") != 0 && strcmp(names[i], "..") != 0)
            count++;
    }
    if (count > 0) {
        kprintf("rmdir: '%s' not empty (%d entries)\n", path, count);
        shell_set_exit_status(1);
        return;
    }

    if (fs_delete(path) < 0) {
        kprintf("rmdir: cannot remove '%s'\n", path);
        shell_set_exit_status(1);
        return;
    }
    shell_set_exit_status(0);
}
