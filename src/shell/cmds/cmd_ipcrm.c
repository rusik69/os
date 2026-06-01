#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ipcrm(const char *args) {
    (void)args;
    kprintf("ipcrm: reading '%s'\n", args ? args : "(stdin)");
}
