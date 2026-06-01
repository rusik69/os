#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_lslocks(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: lslocks <args>\n");
        return;
    }
    kprintf("lslocks: %s\n", args);
}
