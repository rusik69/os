#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_colrm(const char *args) {
    (void)args;
    kprintf("colrm: reading '%s'\n", args ? args : "(stdin)");
}
