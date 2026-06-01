#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ipcs(const char *args) {
    (void)args;
    kprintf("ipcs: reading '%s'\n", args ? args : "(stdin)");
}
