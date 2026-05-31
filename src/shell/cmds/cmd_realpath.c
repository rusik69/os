/* cmd_realpath.c — print resolved absolute path */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

static void resolve_path(const char *in, char *out, int outsz) {
    char tmp[128];
    strncpy(tmp, in, 127);
    tmp[127] = '\0';

    /* Start with root */
    out[0] = '/';
    out[1] = '\0';
    int opos = 1;

    char *p = tmp;
    char comp[64];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        int ci = 0;
        while (*p && *p != '/' && ci < 63) comp[ci++] = *p++;
        comp[ci] = '\0';

        if (strcmp(comp, ".") == 0) {
            /* skip */
        } else if (strcmp(comp, "..") == 0) {
            /* go up one level */
            if (opos > 1) {
                opos--;
                while (opos > 0 && out[opos-1] != '/') opos--;
                if (opos == 0) { out[0] = '/'; opos = 1; }
                out[opos] = '\0';
            }
        } else {
            /* append component */
            if (opos > 1) { out[opos++] = '/'; }
            int ci2 = 0;
            while (comp[ci2] && opos < outsz-1) out[opos++] = comp[ci2++];
            out[opos] = '\0';
        }
    }
    if (opos == 0) { out[0] = '/'; out[1] = '\0'; }
}

void cmd_realpath(const char *args) {
    if (!args || !*args) { kprintf("Usage: realpath <path>\n"); return; }

    char path[128];
    strncpy(path, args, 127);
    path[127] = '\0';
    /* Trim trailing spaces */
    int len = strlen(path);
    while (len > 0 && path[len-1] == ' ') path[--len] = '\0';

    /* If relative, prepend CWD */
    char full[128];
    if (path[0] != '/') {
        char cwd[64];
        if (libc_getcwd(cwd, sizeof(cwd)) < 0)
            strcpy(cwd, "/");
        int cl = strlen(cwd);
        if (cwd[cl-1] == '/') cl--;
        strncpy(full, cwd, cl);
        full[cl] = '/';
        strncpy(full+cl+1, path, 126-cl);
        full[127] = '\0';
    } else {
        strncpy(full, path, 127);
        full[127] = '\0';
    }

    char resolved[128];
    resolve_path(full, resolved, sizeof(resolved));
    kprintf("%s\n", resolved);
}
