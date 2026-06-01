#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_setsid(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: setsid <args>\n");
        return;
    }
    kprintf("setsid: %s\n", args);
}
