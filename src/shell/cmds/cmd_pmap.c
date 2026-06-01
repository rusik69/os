#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_pmap(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: pmap <args>\n");
        return;
    }
    kprintf("pmap: %s\n", args);
}
