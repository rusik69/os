#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lsattr(const char *args) {
    (void)args;
    kprintf("lsattr: reading '%s'\n", args ? args : "(stdin)");
}
