#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_mesg(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: mesg <args>\n");
        return;
    }
    kprintf("mesg: %s\n", args);
}
