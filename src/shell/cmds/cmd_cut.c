/* cmd_cut.c — Extract fields from text */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_cut(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: cut -d<delim> -f<field> <file>\n");
        return;
    }

    char delim = '\t';
    int field = 1;
    const char *p = args;

    /* Parse options */
    while (*p == '-') {
        p++;
        if (*p == 'd') {
            p++;
            if (*p) delim = *p++;
        } else if (*p == 'f') {
            p++;
            field = 0;
            while (*p >= '0' && *p <= '9') { field = field * 10 + (*p - '0'); p++; }
        }
        while (*p == ' ') p++;
    }
    if (field < 1) field = 1;

    if (!*p) { kprintf("cut: no file specified\n"); return; }

    char path[64];
    if (*p != '/') { path[0] = '/'; strncpy(path + 1, p, 62); }
    else strncpy(path, p, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4095, &size) != 0) {
        kprintf("cut: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    /* Process line by line */
    char *line = buf;
    for (uint32_t i = 0; i <= size; i++) {
        if (buf[i] == '\n' || i == size) {
            buf[i] = '\0';
            /* Extract field */
            char *fp = line;
            int cur = 1;
            while (cur < field && *fp) {
                if (*fp == delim) cur++;
                fp++;
            }
            if (cur == field) {
                /* Print until next delim or end */
                while (*fp && *fp != delim) {
                    kprintf("%c", (uint64_t)(uint8_t)*fp);
                    fp++;
                }
            }
            kprintf("\n");
            line = &buf[i + 1];
        }
    }
}
