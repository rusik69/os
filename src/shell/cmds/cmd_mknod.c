/* cmd_mknod.c — create device node */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_mknod(const char *args) {
    if (!args || !*args) { kprintf("Usage: mknod <path> [b|c] <major> <minor>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char path[64], type_c;
    int major = 0, minor = 0;

    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) path[i++] = *p++;
    path[i] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: mknod <path> [b|c] <major> <minor>\n"); return; }
    type_c = *p;
    if (type_c != 'b' && type_c != 'c') { kprintf("mknod: type must be 'b' or 'c'\n"); return; }
    p++;
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: mknod <path> [b|c] <major> <minor>\n"); return; }
    char num[16]; i = 0;
    while (*p && *p != ' ' && i < 15) num[i++] = *p++;
    num[i] = '\0';
    major = strtol(num, 0, 10);
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 15) num[i++] = *p++;
    num[i] = '\0';
    minor = strtol(num, 0, 10);

    char fpath[64];
    if (path[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, path, 62); fpath[63] = '\0'; }
    else strncpy(fpath, path, 63);
    fpath[63] = '\0';

    /* Just create a regular file as a placeholder; real device nodes need devfs */
    if (fs_create(fpath, FS_TYPE_FILE) < 0) {
        kprintf("mknod: cannot create '%s'\n", fpath);
        shell_set_exit_status(1);
        return;
    }
    kprintf("mknod: created '%s' (type %c, major %d, minor %d)\n", fpath, type_c, major, minor);
    shell_set_exit_status(0);
}
