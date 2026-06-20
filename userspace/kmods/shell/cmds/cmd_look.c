/* cmd_look.c — display lines beginning with a given string */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_look(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: look <prefix> [file]\n");
        return;
    }

    /* Parse: first token is prefix, second (optional) is file */
    char prefix[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) prefix[i++] = *p++;
    prefix[i] = '\0';

    while (*p == ' ') p++; /* skip spaces */
    const char *filename = p;

    char fpath[64];
    if (!filename || !filename[0]) {
        /* Default: read from /etc/passwd if exists */
        strncpy(fpath, "/etc/passwd", sizeof(fpath) - 1);
        fpath[sizeof(fpath) - 1] = '\0';
    } else {
        if (filename[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, filename, 62); }
        else strncpy(fpath, filename, 63);
        fpath[63] = '\0';
    }

    static char buf[8192];
    uint32_t size = 0;
    if (vfs_read(fpath, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("look: cannot read '%s'\n", fpath);
        return;
    }
    buf[size] = '\0';

    /* Print lines starting with prefix */
    char *line = buf;
    for (uint32_t idx = 0; idx <= size; idx++) {
        if (buf[idx] == '\n' || idx == size) {
            char save = buf[idx];
            buf[idx] = '\0';
            if (strncmp(line, prefix, strlen(prefix)) == 0) {
                kprintf("%s\n", line);
            }
            buf[idx] = save;
            line = buf + idx + 1;
        }
    }
}
