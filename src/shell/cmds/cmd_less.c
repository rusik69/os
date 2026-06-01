/* cmd_less.c — page through a file (like more) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "keyboard.h"

#define LESS_LINES 24

void cmd_less(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: less <file>\n");
        return;
    }

    char path[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) path[i++] = *p++;
    path[i] = '\0';

    char fpath[64];
    if (path[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, path, 62); }
    else strncpy(fpath, path, 63);
    fpath[63] = '\0';

    static char buf[8192];
    uint32_t size = 0;
    if (vfs_read(fpath, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("less: cannot read '%s'\n", fpath);
        return;
    }
    buf[size] = '\0';

    int lines_shown = 0;
    char *line = buf;
    for (uint32_t idx = 0; idx <= size; idx++) {
        if (buf[idx] == '\n' || idx == size) {
            char save = buf[idx];
            buf[idx] = '\0';
            kprintf("%s\n", line);
            buf[idx] = save;
            line = buf + idx + 1;
            lines_shown++;
            if (lines_shown >= LESS_LINES && idx < size) {
                kprintf("--More--(q=quit)");
                char c = keyboard_getchar();
                if (c == 'q' || c == 'Q') break;
                kprintf("\r                \r");
                lines_shown = 0;
            }
        }
    }
}
