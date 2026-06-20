/* cmd_readlink.c — print symlink target */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_readlink(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: readlink <path>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') {
        path[0] = '/'; strncpy(path + 1, args, 62); path[63] = '\0';
    } else {
        strncpy(path, args, 63); path[63] = '\0';
    }
    char buf[256];
    int r = libc_fs_readlink(path, buf, sizeof(buf));
    if (r < 0) {
        kprintf("readlink: %s: not a symlink\n", args);
        return;
    }
    kprintf("%s\n", buf);
}
