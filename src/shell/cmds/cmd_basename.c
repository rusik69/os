/* cmd_basename.c — basename and dirname commands */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_basename(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: basename <path>\n");
        return;
    }

    /* Trim trailing spaces */
    char path[128];
    strncpy(path, args, 127);
    path[127] = '\0';
    int len = strlen(path);
    while (len > 0 && path[len-1] == ' ') path[--len] = '\0';

    /* Remove trailing slashes */
    while (len > 1 && path[len-1] == '/') path[--len] = '\0';

    /* Find last slash */
    char *slash = strrchr(path, '/');
    char *last = slash ? slash + 1 : path;
    if (path[0] == '/' && !path[1]) last = path; /* root */

    kprintf("%s\n", last);
}

void cmd_dirname(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: dirname <path>\n");
        return;
    }

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
        /* Remove trailing slash (but keep root /) */
        if (len > 1) len--;
        path[len] = '\0';
        kprintf("%s\n", path);
    }
}
