#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_tset(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: tset <args>\n");
        return;
    }
    kprintf("tset: %s\n", args);
}
