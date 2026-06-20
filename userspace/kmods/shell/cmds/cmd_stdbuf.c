#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_stdbuf(const char *args) {
    if (!args) { kprintf("Usage: stdbuf -i0 -oL command\n"); return; }
    kprintf("stdbuf: configuring buffers\n");
}
