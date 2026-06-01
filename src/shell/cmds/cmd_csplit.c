#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_csplit(const char *args) {
    (void)args;
    kprintf("csplit: reading '%s'\n", args ? args : "(stdin)");
}
