/* cmd_fold.c — wrap long lines to a given width */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_fold(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: fold [-w width] <file>\n");
        return;
    }

    int width = 80;
    const char *p = args;

    /* Parse -w WIDTH */
    while (*p == '-') {
        p++;
        if (*p == 'w') {
            p++;
            while (*p == ' ') p++;
            width = (int)strtol(p, (char **)&p, 10);
            if (width < 1) width = 80;
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    if (!*p) { kprintf("fold: no file specified\n"); return; }

    char path[64];
    if (*p != '/') { path[0] = '/'; strncpy(path + 1, p, 62); }
    else strncpy(path, p, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("fold: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    int col = 0;
    for (uint32_t i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\n') {
            kprintf("\n");
            col = 0;
        } else {
            if (col >= width) { kprintf("\n"); col = 0; }
            kprintf("%c", c);
            col++;
        }
    }
    if (col > 0) kprintf("\n");
}
