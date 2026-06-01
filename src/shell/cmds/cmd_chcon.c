#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_chcon(const char *args) {
    (void)args;
    kprintf("chcon: reading '%s'\n", args ? args : "(stdin)");
}
