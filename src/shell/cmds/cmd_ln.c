/* cmd_ln.c — create a file link (copy, since VFS has no symlinks) */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

void cmd_ln(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: ln <source> <dest>\n");
        return;
    }

    char src[64], dst[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) src[i++] = *p++;
    src[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) dst[i++] = *p++;
    dst[i] = '\0';

    if (!src[0] || !dst[0]) { kprintf("Usage: ln <source> <dest>\n"); return; }

    char s[64], d[64];
    if (src[0] != '/') { s[0] = '/'; strncpy(s+1, src, 62); } else strncpy(s, src, 63);
    s[63] = '\0';
    if (dst[0] != '/') { d[0] = '/'; strncpy(d+1, dst, 62); } else strncpy(d, dst, 63);
    d[63] = '\0';

    static char buf[4096];
    uint32_t size = 0;
    if (vfs_read(s, buf, sizeof(buf)-1, &size) != 0) {
        kprintf("ln: cannot read '%s'\n", s);
        return;
    }
    vfs_create(d, 1);
    if (vfs_write(d, buf, size) != 0) {
        kprintf("ln: cannot write '%s'\n", d);
        return;
    }
    kprintf("'%s' -> '%s'\n", s, d);
}
