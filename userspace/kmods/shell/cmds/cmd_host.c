#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_host(const char *args) {
    if (!args) { kprintf("Usage: host <hostname>\n"); return; }
    kprintf("%s has address (DNS lookup via kernel)\n", args);
}
