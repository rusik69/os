/* cmd_tset.c — Set terminal settings (stub) */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"

void cmd_tset(const char *args) {
    (void)args;
    kprintf("tset: terminal settings applied\n");
}
