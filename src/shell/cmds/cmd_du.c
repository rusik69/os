/* cmd_du.c — Disk usage for files */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_du(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: du <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("du: cannot stat '%s'\n", path);
        return;
    }

    /* Show size in blocks (512-byte blocks) and bytes */
    uint32_t blocks = (st.size + 511) / 512;
    kprintf("%u\t%s\n", (uint64_t)blocks, path);
}
