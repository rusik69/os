/* cmd_dirname.c — strip last component from file path */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_dirname(const char *args) {
    if (!args || !*args) { kprintf("Usage: dirname <path>\n"); return; }

    char path[128];
    strncpy(path, args, 127);
    path[127] = '\0';
    int len = strlen(path);
    while (len > 0 && path[len-1] == ' ') path[--len] = '\0';

    /* Remove trailing slashes */
    while (len > 1 && path[len-1] == '/') path[--len] = '\0';

    /* Remove last component */
    while (len > 0 && path[len-1] != '/') len--;
    if (len == 0) {
        kprintf(".\n");
    } else {
        if (len > 1) len--;
        path[len] = '\0';
        kprintf("%s\n", path);
    }
}
