/* cmd_run.c — run command */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_run(const char *args) {
    if (!args || !*args) { kprintf("Usage: run <script>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/')
        snprintf(path, sizeof(path), "/%s", args);
    else
        snprintf(path, sizeof(path), "%s", args);
    script_exec(path);
}
