#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_chcon(const char *args) {
    if (!args) { kprintf("Usage: chcon <context> <file>\n"); return; }
    kprintf("chcon: not supported on this system\n");
}
