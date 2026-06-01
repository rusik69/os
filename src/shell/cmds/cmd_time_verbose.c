#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_time_verbose(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: time_verbose <args>\n");
        return;
    }
    kprintf("time_verbose: %s\n", args);
}
