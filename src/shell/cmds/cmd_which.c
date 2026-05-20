/* cmd_which.c — check if a shell command is known */
#include "shell_cmds.h"
#include "shell_cmd_table.h"
#include "printf.h"
#include "string.h"

void cmd_which(const char *args) {
    if (!args || !*args) { kprintf("Usage: which <command>\n"); return; }

    char name[64];
    int i = 0;
    while (args[i] && args[i] != ' ' && i < 63) { name[i] = args[i]; i++; }
    name[i] = '\0';

    if (strcmp(name, "[") == 0 || shell_cmd_exists(name)) {
        kprintf("%s: shell built-in\n", name);
        return;
    }
    kprintf("%s: not found\n", name);
}
