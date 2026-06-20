/* cmd_format.c — format command */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_format_disk(void) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    if (fs_format() < 0) kprintf("Format failed\n");
    else kprintf("Filesystem formatted\n");
}
