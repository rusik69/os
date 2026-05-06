/* cmd_cat.c — cat command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_cat(const char *args) {
    if (!args) { kprintf("Usage: cat <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(args, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read file: %s\n", args);
        return;
    }
    fbuf[size] = '\0';
    kprintf("%s\n", fbuf);
}
