#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_localedef(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: localedef <args>\n");
        return;
    }
    kprintf("localedef: %s\n", args);
}
