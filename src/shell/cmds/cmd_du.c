/* cmd_du.c — Disk usage for files */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_du(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: du <file>\n");
        return;
    }

    static char path[64];
    strncpy(path, args, 63);
    path[63] = '\0';
    if (path[0] != '/') {
        memmove(path + 1, path, strlen(path) + 1);
        path[0] = '/';
    }
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("du: cannot stat '%s'\n", path);
        return;
    }

    uint32_t blocks = (st.size + 511) / 512;
    kprintf("%u %s\n", (uint64_t)blocks, path);
}
