#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_moc(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: moc <args>\n");
        return;
    }
    kprintf("moc: %s\n", args);
}
