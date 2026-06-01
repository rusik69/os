/* cmd_truncate.c — shrink or extend file to specified size */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_truncate(const char *args) {
    if (!args || !*args) { kprintf("Usage: truncate <file> <size>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char fname[64], sstr[16];
    const char *p = args;
    int i = 0;
    while (*p && *p != ' ' && i < 63) fname[i++] = *p++;
    fname[i] = '\0';
    while (*p == ' ') p++;
    i = 0;
    while (*p && *p != ' ' && i < 15) sstr[i++] = *p++;
    sstr[i] = '\0';

    if (!fname[0] || !sstr[0]) { kprintf("Usage: truncate <file> <size>\n"); return; }

    char path[64];
    if (fname[0] != '/') { path[0] = '/'; strncpy(path+1, fname, 62); path[63] = '\0'; }
    else strncpy(path, fname, 63);
    path[63] = '\0';

    uint32_t newsize = (uint32_t)strtol(sstr, 0, 10);

    if (libc_truncate(path, (uint32_t)newsize) < 0) {
        kprintf("truncate: cannot truncate '%s' to %u bytes\n", fname, (unsigned long)newsize);
        shell_set_exit_status(1);
        return;
    }
    kprintf("truncate: %s truncated to %u bytes\n", fname, (unsigned long)newsize);
    shell_set_exit_status(0);
}
