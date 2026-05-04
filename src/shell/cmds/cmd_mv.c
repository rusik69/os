/* cmd_mv.c — mv command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "fs.h"
#include "ata.h"

void cmd_mv(const char *args) {
    if (!args) { kprintf("Usage: mv <src> <dst>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char src[64], dst[64];
    int si = 0;
    while (*p && *p != ' ' && si < 63) src[si++] = *p++;
    src[si] = '\0';
    while (*p == ' ') p++;
    int di = 0;
    while (*p && *p != ' ' && di < 63) dst[di++] = *p++;
    dst[di] = '\0';
    if (!dst[0]) { kprintf("Usage: mv <src> <dst>\n"); return; }
    char spath[64], dpath[64];
    if (src[0] != '/') { spath[0] = '/'; strcpy(spath + 1, src); }
    else strcpy(spath, src);
    if (dst[0] != '/') { dpath[0] = '/'; strcpy(dpath + 1, dst); }
    else strcpy(dpath, dst);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(spath, fbuf, sizeof(fbuf), &size) < 0) {
        kprintf("Cannot read: %s\n", src);
        return;
    }
    uint32_t ds; uint8_t dt;
    if (fs_stat(dpath, &ds, &dt) < 0) {
        if (fs_create(dpath, FS_TYPE_FILE) < 0) {
            kprintf("Cannot create: %s\n", dst);
            return;
        }
    }
    if (fs_write_file(dpath, fbuf, size) < 0) {
        kprintf("Write failed: %s\n", dst);
        return;
    }
    fs_delete(spath);
    kprintf("Moved: %s -> %s\n", src, dst);
}
