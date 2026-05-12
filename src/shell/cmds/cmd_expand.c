/* cmd_expand.c — convert tabs to spaces */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

void cmd_expand(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: expand [-t tabstop] <file>\n");
        return;
    }

    int tabstop = 8;
    const char *p = args;

    if (*p == '-' && *(p+1) == 't') {
        p += 2;
        while (*p == ' ') p++;
        tabstop = (int)strtol(p, (char **)&p, 10);
        if (tabstop < 1) tabstop = 8;
        while (*p == ' ') p++;
    }

    if (!*p) { kprintf("expand: no file specified\n"); return; }

    char path[64];
    if (*p != '/') { path[0] = '/'; strncpy(path + 1, p, 62); }
    else strncpy(path, p, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf) - 1, &size) != 0) {
        kprintf("expand: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    int col = 0;
    for (uint32_t i = 0; i < size; i++) {
        char c = buf[i];
        if (c == '\t') {
            int spaces = tabstop - (col % tabstop);
            for (int s = 0; s < spaces; s++) { kprintf(" "); col++; }
        } else {
            kprintf("%c", c);
            col = (c == '\n') ? 0 : col + 1;
        }
    }
}
