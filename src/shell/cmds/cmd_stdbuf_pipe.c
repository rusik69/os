#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_stdbuf_pipe(const char *args) {
    if (!args || strlen(args) == 0) {
        kprintf("Usage: stdbuf_pipe <args>\n");
        return;
    }
    kprintf("stdbuf_pipe: %s\n", args);
}
