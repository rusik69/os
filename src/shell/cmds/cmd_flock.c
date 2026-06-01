#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_flock(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: flock <args>\n");
        return;
    }
    kprintf("flock: %s\n", args);
}
