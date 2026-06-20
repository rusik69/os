/* cmd_exec.c — exec command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_exec(const char *args) {
    if (!args || !*args) { kprintf("Usage: exec <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    static char path[64];
    if (args[0] != '/') { path[0] = '/'; path[1] = '\0'; strncpy(path + 1, args, 62); path[63] = '\0'; }
    else { strncpy(path, args, 63); path[63] = '\0'; }
    elf_exec(path);
}
