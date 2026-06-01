#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_basenc(const char *args) {
    (void)args;
    kprintf("basenc: reading '%s'\n", args ? args : "(stdin)");
}
