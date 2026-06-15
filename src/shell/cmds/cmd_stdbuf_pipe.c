#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_stdbuf_pipe(const char *args) {
    if (!args) { kprintf("Usage: stdbuf_pipe <cmd>\n"); return; }
    kprintf("stdbuf_pipe: configuring pipe buffer for '%s'\n", args);
}
