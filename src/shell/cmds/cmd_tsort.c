/* cmd_tsort.c — topological sort */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define MAX_ITEMS 256
#define MAX_NAME 64

static int find_or_add(char items[MAX_ITEMS][MAX_NAME], int *n, const char *name) {
    for (int i = 0; i < *n; i++)
        if (strcmp(items[i], name) == 0) return i;
    strncpy(items[*n], name, MAX_NAME-1);
    items[*n][MAX_NAME-1] = '\0';
    return (*n)++;
}

void cmd_tsort(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: tsort <file>\n");
        return;
    }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strncpy(path+1, args, 62); }
    else strncpy(path, args, 63);
    path[63] = '\0';
    int pl = strlen(path);
    while (pl > 0 && path[pl-1] == ' ') path[--pl] = '\0';

    static char buf[8192];
    uint32_t size = 0;
    if (vfs_read(path, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("tsort: cannot read '%s'\n", path);
        return;
    }
    buf[size] = '\0';

    char items[MAX_ITEMS][MAX_NAME];
    int deps[MAX_ITEMS][MAX_ITEMS];
    int deg[MAX_ITEMS];
    int n = 0;

    memset(deps, 0, sizeof(deps));
    memset(deg, 0, sizeof(deg));

    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (!*p) break;
        char a[MAX_NAME], b[MAX_NAME];
        int ai = 0, bi = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && ai < MAX_NAME-1)
            a[ai++] = *p++;
        a[ai] = '\0';
        while (*p == ' ' || *p == '\t') p++;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && bi < MAX_NAME-1)
            b[bi++] = *p++;
        b[bi] = '\0';
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        if (!a[0] || !b[0]) continue;

        int ia = find_or_add(items, &n, a);
        int ib = find_or_add(items, &n, b);
        if (!deps[ia][ib]) {
            deps[ia][ib] = 1;
            deg[ib]++;
        }
    }

    /* Kahn's algorithm */
    int sorted = 0;
    while (sorted < n) {
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (deg[i] == 0) {
                deg[i] = -1;
                kprintf("%s\n", items[i]);
                sorted++;
                found = 1;
                for (int j = 0; j < n; j++)
                    if (deps[i][j]) deg[j]--;
                break;
            }
        }
        if (!found) {
            kprintf("tsort: cycle detected\n");
            shell_set_exit_status(1);
            return;
        }
    }
    shell_set_exit_status(0);
}
