/* cmd_cat.c — cat command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_cat(const char *args) {
    /* No filename: try piped stdin */
    if (!args || !args[0]) {
        if (!shell_has_stdin()) { kprintf("Usage: cat <file>\n"); return; }
        static char sbuf[32768];
        int slen = shell_stdin_read(sbuf, (int)sizeof(sbuf) - 1);
        sbuf[slen] = '\0';
        kprintf("%s", sbuf);
        return;
    }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    static char fbuf[4096];
    uint32_t size;
    if (vfs_read(args, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read file: %s\n", args);
        return;
    }
    fbuf[size] = '\0';
    kprintf("%s\n", fbuf);
}
