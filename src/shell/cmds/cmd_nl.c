/* cmd_nl.c — Number lines of a file */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_nl(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: nl <file>\n");
        return;
    }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4095, &size) != 0) {
        kprintf("nl: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    int line_num = 1;
    char *line = buf;
    for (uint32_t i = 0; i <= size; i++) {
        if (buf[i] == '\n' || i == size) {
            buf[i] = '\0';
            kprintf("%6d\t%s\n", (uint64_t)line_num, line);
            line_num++;
            line = &buf[i + 1];
        }
    }
}
