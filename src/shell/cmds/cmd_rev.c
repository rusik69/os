/* cmd_rev.c — Reverse lines of a file or argument */

#include "shell_cmds.h"
#include "vfs.h"
#include "printf.h"
#include "string.h"

void cmd_rev(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: rev <file|string>\n");
        return;
    }

    /* Try as file first */
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    const char *text;
    uint32_t text_len;

    if (vfs_read(path, buf, 4095, &size) == 0) {
        buf[size] = '\0';
        text = buf;
        text_len = size;
    } else {
        /* Treat as literal string */
        text = args;
        text_len = strlen(args);
    }

    /* Reverse each line */
    const char *line = text;
    for (uint32_t i = 0; i <= text_len; i++) {
        if (text[i] == '\n' || i == text_len) {
            int llen = &text[i] - line;
            for (int j = llen - 1; j >= 0; j--)
                kprintf("%c", (uint64_t)(uint8_t)line[j]);
            kprintf("\n");
            line = &text[i + 1];
        }
    }
}
