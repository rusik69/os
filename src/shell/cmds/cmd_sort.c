/* cmd_sort.c -- Sort lines of a file alphabetically */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"    /* rand, qsort */

static int cmp_strptr(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

void cmd_sort(const char *args) {
    int reverse = 0, shuffle = 0;
    const char *p = args ? args : "";

    while (*p == '-') {
        p++;
        while (*p && *p != ' ') {
            if (*p == 'r') reverse = 1;
            else if (*p == 'R') shuffle = 1;
            p++;
        }
        while (*p == ' ') p++;
    }

    static char buf[4096];
    uint32_t size = 0;

    if (!*p) {
        if (!shell_has_stdin()) { kprintf("Usage: sort [-r] [-R] <file>\n"); return; }
        size = (uint32_t)shell_stdin_read(buf, (int)sizeof(buf) - 1);
    } else {
        char path[64];
        snprintf(path, sizeof(path), "%s%s", p[0] == '/' ? "" : "/", p);
        strtrim(path);
        if (vfs_read(path, buf, 4095, &size) != 0) {
            kprintf("sort: cannot read '%s'\n", path);
            return;
        }
    }
    buf[size] = '\0';

    /* Split into lines */
    char *lines[256];
    int nlines = 0;
    char *q = buf;
    while (*q && nlines < 256) {
        lines[nlines++] = q;
        while (*q && *q != '\n') q++;
        if (*q == '\n') { *q = '\0'; q++; }
    }

    if (shuffle) {
        for (int i = nlines - 1; i > 0; i--) {
            int j = rand() % (i + 1);
            char *tmp = lines[i]; lines[i] = lines[j]; lines[j] = tmp;
        }
    } else {
        qsort(lines, (size_t)nlines, sizeof(char *), cmp_strptr);
        if (reverse) {
            for (int i = 0, j = nlines-1; i < j; i++, j--) {
                char *tmp = lines[i]; lines[i] = lines[j]; lines[j] = tmp;
            }
        }
    }

    for (int i = 0; i < nlines; i++)
        kprintf("%s\n", lines[i]);
}
