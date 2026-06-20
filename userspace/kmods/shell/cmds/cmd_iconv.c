/* cmd_iconv.c — convert character sets (simple copy) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_iconv(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: iconv <file>  (default: copy to stdout)\n");
        return;
    }

    /* Simple: read file and output it (passthrough) */
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
        kprintf("iconv: cannot read '%s'\n", fpath);
        return;
    }
    buf[size] = '\0';
    kprintf("%s", buf);
}
