/* cmd_mkfifo.c — Create a FIFO/named pipe */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_mkfifo(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: mkfifo <name>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    /* Create a file with type marker for FIFO (type 3) */
    int ret = vfs_create(path, 3);
    if (ret < 0) {
        kprintf("mkfifo: cannot create '%s'\n", path);
        return;
    }
    kprintf("mkfifo: created '%s'\n", path);
}
