/* cmd_grep.c — grep command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_grep(const char *args) {
    if (!args) { kprintf("Usage: grep <pattern> <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char pattern[128];
    int pi = 0;
    while (*p && *p != ' ' && pi < 127) pattern[pi++] = *p++;
    pattern[pi] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: grep <pattern> <file>\n"); return; }
    char path[64];
    if (*p != '/') { path[0] = '/'; strcpy(path + 1, p); }
    else strcpy(path, p);
    int pl = strlen(path);
    while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", p);
        return;
    }
    fbuf[size] = '\0';
    char *line = fbuf;
    int count = 0;
    for (uint32_t i = 0; i <= size; i++) {
        if (fbuf[i] == '\n' || i == size) {
            fbuf[i] = '\0';
            if (strstr(line, pattern)) {
                kprintf("%s\n", line);
                count++;
            }
            line = &fbuf[i + 1];
        }
    }
    if (count == 0) kprintf("No matches\n");
}
