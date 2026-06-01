#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_runcon(const char *args) {
    (void)args;
    kprintf("runcon: reading '%s'\n", args ? args : "(stdin)");
}
