#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_dircolors(const char *args) {
    (void)args;
    kprintf("LS_COLORS='di=01;34:ln=01;36:ex=01;32:'\n");
}
