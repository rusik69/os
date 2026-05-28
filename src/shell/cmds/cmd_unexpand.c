/* cmd_unexpand.c — convert spaces to tabs */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_unexpand(const char *args) {
    int tabstop = 8;
    const char *p = args;
    if (!args || !args[0]) {
        kprintf("Usage: unexpand [-t <n>] <file>\n");
        return;
    }
    while (*p == '-') {
        p++;
        if (*p == 't') {
            p++;
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n * 10 + (*p - '0'); p++; }
            if (n > 0) tabstop = n;
        }
        while (*p == ' ') p++;
    }
    char path[64];
    int i = 0;
    while (*p && *p != ' ' && i < 63) path[i++] = *p++;
    path[i] = '\0';
    if (!path[0]) { kprintf("unexpand: need a file\n"); return; }
    char fpath[64];
    if (path[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, path, 62); }
    else strncpy(fpath, path, 63);
    fpath[63] = '\0';

    static char buf[8192];
    uint32_t size = 0;
    if (vfs_read(fpath, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("unexpand: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    int col = 0;
    int spaces = 0;
    for (uint32_t pos = 0; pos <= size; pos++) {
        char c = buf[pos];
        if (c == ' ') {
            spaces++;
            col++;
            if (col % tabstop == 0) {
                kprintf("\t");
                spaces = 0;
            }
        } else {
            while (spaces > 0) { kprintf(" "); spaces--; }
            if (c == '\0') break;
            kprintf("%c", c);
            col = (c == '\n') ? 0 : col + 1;
        }
    }
}
