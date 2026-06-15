#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_logger(const char *args) {
    if (!args) { kprintf("Usage: logger <message>\n"); return; }
    kprintf("<6>%s\n", args);
}
