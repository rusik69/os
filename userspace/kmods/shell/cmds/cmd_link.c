/* cmd_link.c — create a hard link (same inode via symlink target) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_link(const char *args) {
    if (!args || !*args) { kprintf("Usage: link <existing> <new>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char src[64], dst[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) src[i++] = *p++;
    src[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) dst[i++] = *p++;
    dst[i] = '\0';

    if (!src[0] || !dst[0]) { kprintf("Usage: link <existing> <new>\n"); return; }

    char spath[64], dpath[64];
    if (src[0] != '/') { spath[0] = '/'; strncpy(spath+1, src, 62); spath[63] = '\0'; }
    else strncpy(spath, src, 63);
    spath[63] = '\0';
    if (dst[0] != '/') { dpath[0] = '/'; strncpy(dpath+1, dst, 62); dpath[63] = '\0'; }
    else strncpy(dpath, dst, 63);
    dpath[63] = '\0';

    if (libc_fs_symlink(dpath, spath) < 0) {
        kprintf("link: cannot create link '%s' -> '%s'\n", dpath, spath);
        shell_set_exit_status(1);
        return;
    }
    kprintf("link: '%s' -> '%s'\n", dpath, spath);
    shell_set_exit_status(0);
}
