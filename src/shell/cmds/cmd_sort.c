/* cmd_sort.c — Sort lines of a file alphabetically */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"

static int cmp_strptr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cmd_sort(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: sort <file>\n"); return; }

    char path[64];
    snprintf(path, sizeof(path), "%s%s", args[0] == '/' ? "" : "/", args);
    strtrim(path);

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4095, &size) != 0) {
        kprintf("sort: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    /* Split into lines */
    char *lines[256];
    int nlines = 0;
    char *p = buf;
    while (*p && nlines < 256) {
        lines[nlines++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = '\0'; p++; }
    }

    /* Sort with qsort */
    qsort(lines, (size_t)nlines, sizeof(char *), cmp_strptr);

    for (int i = 0; i < nlines; i++)
        kprintf("%s\n", lines[i]);
}
