/* cmd_pr.c — paginate/file print (simple column formatter) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_pr(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: pr <file>\n");
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
        kprintf("pr: cannot read '%s'\n", fpath);
        return;
    }
    buf[size] = '\0';

    /* Add page headers and line numbers */
    int line_no = 1;
    int page_no = 1;
    kprintf("=== Page %d ===\n", page_no);
    char *line = buf;
    for (uint32_t idx = 0; idx < size; idx++) {
        if (buf[idx] == '\n') {
            buf[idx] = '\0';
            kprintf("  %4d  %s\n", line_no, line);
            buf[idx] = '\n';
            line = buf + idx + 1;
            line_no++;
            if (line_no % 66 == 0) {
                page_no++;
                kprintf("=== Page %d ===\n", page_no);
            }
        }
    }
    /* Last line if not empty */
    if (line < buf + size) {
        kprintf("  %4d  %s\n", line_no, line);
    }

    (void)page_no;
}
