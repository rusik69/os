#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_zipnote(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: zipnote <args>\n");
        return;
    }
    kprintf("zipnote: %s\n", args);
}
