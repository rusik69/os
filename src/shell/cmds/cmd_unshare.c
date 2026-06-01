#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_unshare(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: unshare <args>\n");
        return;
    }
    kprintf("unshare: %s\n", args);
}
