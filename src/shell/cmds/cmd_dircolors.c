#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_dircolors(const char *args) {
    (void)args;
    kprintf("dircolors: reading '%s'\n", args ? args : "(stdin)");
}
