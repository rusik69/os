/* cmd_uniq.c — Remove adjacent duplicate lines */

#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_uniq(const char *args) {
    if (!args || !args[0]) { kprintf("Usage: uniq <file>\n"); return; }

    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(path, buf, 4095, &size) != 0) {
        kprintf("uniq: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    /* Print lines, skipping adjacent duplicates */
    char prev[256]; prev[0] = '\0';
    char *p = buf;
    while (*p) {
        char line[256];
        int i = 0;
        while (*p && *p != '\n' && i < 255) line[i++] = *p++;
        line[i] = '\0';
        if (*p == '\n') p++;

        if (strcmp(line, prev) != 0) {
            kprintf("%s\n", line);
            strncpy(prev, line, 255);
        }
    }
}
