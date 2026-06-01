/* cmd_shuf.c — shuffle/randomize lines of a file */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_shuf(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: shuf <file>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path + 1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[8192];
    char *lines[512];
    int nlines = 0;
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("shuf: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    char *p = buf;
    while (*p && nlines < 512) {
        lines[nlines++] = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = '\0';
    }
    if (nlines == 0) return;

    /* Fisher-Yates shuffle */
    for (int i = nlines - 1; i > 0; i--) {
        int j = (int)((unsigned int)rand() % (unsigned int)(i + 1));
        char *tmp = lines[i];
        lines[i] = lines[j];
        lines[j] = tmp;
    }
    for (int i = 0; i < nlines; i++)
        kprintf("%s\n", lines[i]);
}
