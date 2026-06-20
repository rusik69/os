/* cmd_chgrp.c — change file group ownership */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_chgrp(const char *args) {
    if (!args || !*args) { kprintf("Usage: chgrp <group> <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char grp[16], fname[64];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 15) grp[i++] = *p++;
    grp[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 63) fname[i++] = *p++;
    fname[i] = '\0';

    if (!grp[0] || !fname[0]) { kprintf("Usage: chgrp <group> <file>\n"); return; }

    char path[64];
    if (fname[0] != '/') { path[0] = '/'; strncpy(path+1, fname, 62); path[63] = '\0'; }
    else strncpy(path, fname, 63);
    path[63] = '\0';

    uint16_t gid = (uint16_t)strtol(grp, 0, 10);
    uint16_t uid = 0; /* keep current owner */

    /* Get current owner first */
    uint32_t size;
    uint8_t type;
    uint16_t old_uid, old_gid;
    uint16_t mode;
    if (fs_stat_ex(path, &size, &type, &old_uid, &old_gid, &mode) == 0)
        uid = old_uid;

    if (fs_chown(path, uid, gid) < 0) {
        kprintf("chgrp: cannot change group of '%s'\n", fname);
        shell_set_exit_status(1);
        return;
    }
    shell_set_exit_status(0);
}
