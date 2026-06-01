#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_zipsplit(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: zipsplit <args>\n");
        return;
    }
    kprintf("zipsplit: %s\n", args);
}
