#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_zipcloak(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: zipcloak <args>\n");
        return;
    }
    kprintf("zipcloak: %s\n", args);
}
