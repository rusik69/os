#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_numfmt(const char *args) {
    (void)args;
    kprintf("numfmt: reading '%s'\n", args ? args : "(stdin)");
}
