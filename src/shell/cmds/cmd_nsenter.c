#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_nsenter(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: nsenter <args>\n");
        return;
    }
    kprintf("nsenter: %s\n", args);
}
