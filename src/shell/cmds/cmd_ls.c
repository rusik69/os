/* cmd_ls.c — ls command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ls(const char *args) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *path = args ? args : "/";

    /* Fast path: single path with no spaces */
    const char *sp = path;
    while (*sp && *sp != ' ') sp++;
    if (!*sp) {
        if (vfs_readdir(path) < 0)
            kprintf("Not a directory or not found\n");
        return;
    }

    /* Multiple space-separated paths (e.g. from glob expansion).
     * For each token: try vfs_readdir (directory), else print its basename. */
    static char tok[128];
    const char *p = path;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        int n = 0;
        while (*p && *p != ' ' && n < (int)sizeof(tok) - 1)
            tok[n++] = *p++;
        tok[n] = '\0';
        if (!n) break;
        if (vfs_readdir(tok) < 0) {
            /* Print the basename */
            const char *base = tok;
            for (int i = 0; tok[i]; i++)
                if (tok[i] == '/') base = tok + i + 1;
            kprintf("%s\n", base);
        }
    }
}
