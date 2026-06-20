#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ss(const char *args) {
    (void)args;
    kprintf("Socket statistics:\n");
    kprintf("  (use kernel shell 'ss' command)\n");
}
