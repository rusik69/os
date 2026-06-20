#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_pmap(const char *args) {
    if (!args) { kprintf("Usage: pmap <pid>\n"); return; }
    kprintf("pmap: showing memory map for pid %s\n", args);
}
