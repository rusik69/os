/* cmd_exec.c — exec command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "elf.h"
#include "ata.h"

void cmd_exec(const char *args) {
    if (!args || !*args) { kprintf("Usage: exec <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    elf_exec(path);
}
