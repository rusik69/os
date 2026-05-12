/* cmd_grep.c — grep command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"
#include "stdlib.h"

void cmd_grep(const char *args) {
    if (!args) { kprintf("Usage: grep [-g] [-i] [-v] <pattern> <file>\n"); return; }

    int glob_mode = 0, invert = 0, ignore_case = 0;
    const char *p = args;

    while (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == 'g') glob_mode = 1;
            else if (*p == 'v') invert = 1;
            else if (*p == 'i') ignore_case = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    char pattern[128];
    int pi = 0;
    while (*p && *p != ' ' && pi < 127) pattern[pi++] = *p++;
    pattern[pi] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: grep [-g] [-i] [-v] <pattern> <file>\n"); return; }

    char path[64];
    if (*p != '/') { path[0] = '/'; strncpy(path + 1, p, 62); }
    else strncpy(path, p, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';

    static char fbuf[4096];
    uint32_t size = 0;
    if (vfs_read(path, fbuf, sizeof(fbuf) - 1, &size) != 0) {
        kprintf("grep: cannot read '%s'\n", path);
        return;
    }
    fbuf[size] = '\0';

    /* Build lowercase pattern for -i */
    char lpat[128];
    if (ignore_case) {
        for (int k = 0; k <= pi; k++) lpat[k] = (char)tolower((unsigned char)pattern[k]);
    }

    char *line = fbuf;
    int count = 0;
    for (uint32_t i = 0; i <= size; i++) {
        if (fbuf[i] == '\n' || i == size) {
            fbuf[i] = '\0';
            int matched;
            if (glob_mode) {
                matched = (fnmatch(pattern, line, 0) == 0);
            } else if (ignore_case) {
                /* lowercase copy of line for case-insensitive search */
                char lline[256];
                int li = 0;
                while (line[li] && li < 255) { lline[li] = (char)tolower((unsigned char)line[li]); li++; }
                lline[li] = '\0';
                matched = (strstr(lline, lpat) != (char *)0);
            } else {
                matched = (strstr(line, pattern) != (char *)0);
            }
            if (matched != invert) {
                kprintf("%s\n", line);
                count++;
            }
            line = &fbuf[i + 1];
        }
    }
    if (count == 0) kprintf("No matches\n");
}
