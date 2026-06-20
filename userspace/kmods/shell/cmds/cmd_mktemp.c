/* cmd_mktemp.c — create temporary file and print its name */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_mktemp(const char *args) {
    if (!args || !*args) { kprintf("Usage: mktemp <template>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }

    char tmpl[64];
    strncpy(tmpl, args, 63);
    tmpl[63] = '\0';

    /* Trim trailing spaces */
    int len = strlen(tmpl);
    while (len > 0 && tmpl[len-1] == ' ') tmpl[--len] = '\0';

    /* Replace X's with random digits */
    uint32_t pid = libc_getpid();
    for (int i = 0; tmpl[i]; i++) {
        if (tmpl[i] == 'X') {
            pid = pid * 1103515245 + 12345;
            tmpl[i] = '0' + ((pid >> 16) & 0x0F);
            if (tmpl[i] > '9') tmpl[i] = 'A' + ((pid >> 8) & 0x0F);
        }
    }

    char fpath[64];
    if (tmpl[0] != '/') { fpath[0] = '/'; strncpy(fpath+1, tmpl, 62); fpath[63] = '\0'; }
    else strncpy(fpath, tmpl, 63);
    fpath[63] = '\0';

    if (fs_create(fpath, FS_TYPE_FILE) < 0) {
        kprintf("mktemp: cannot create '%s'\n", tmpl);
        shell_set_exit_status(1);
        return;
    }
    kprintf("%s\n", fpath);
    shell_set_exit_status(0);
}
