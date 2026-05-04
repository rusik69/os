/* cmd_sort.c — Sort lines of a file alphabetically */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_sort(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: sort <file>\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    /* strip trailing spaces */
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

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

    /* Simple bubble sort */
    for (int i = 0; i < nlines - 1; i++) {
        for (int j = 0; j < nlines - 1 - i; j++) {
            if (strcmp(lines[j], lines[j+1]) > 0) {
                char *tmp = lines[j];
                lines[j] = lines[j+1];
                lines[j+1] = tmp;
            }
        }
    }

    for (int i = 0; i < nlines; i++)
        kprintf("%s\n", lines[i]);
}
