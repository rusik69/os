#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_pax(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: pax <args>\n");
        return;
    }
    kprintf("pax: %s\n", args);
}
