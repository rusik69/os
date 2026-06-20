/* cmd_pathchk.c — check pathname validity/readability */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_pathchk(const char *args) {
    if (!args || !*args) { kprintf("Usage: pathchk <path>\n"); return; }

    char path[128];
    strncpy(path, args, 127);
    path[127] = '\0';
    int len = strlen(path);
    while (len > 0 && path[len-1] == ' ') path[--len] = '\0';

    /* Basic path validity checks */
    if (!path[0]) {
        kprintf("pathchk: empty path\n");
        shell_set_exit_status(1);
        return;
    }
    if (path[0] != '/') {
        kprintf("pathchk: '%s' is not absolute\n", path);
        shell_set_exit_status(1);
        return;
    }

    /* Check if file exists and is readable */
    if (ata_is_present()) {
        uint32_t size;
        uint8_t type;
        if (fs_stat(path, &size, &type) < 0) {
            kprintf("pathchk: '%s' does not exist\n", path);
            shell_set_exit_status(1);
            return;
        }
    }

    kprintf("pathchk: '%s' is valid\n", path);
    shell_set_exit_status(0);
}
