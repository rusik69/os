#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lessecho(const char *args) {
    (void)args;
    kprintf("lessecho: reading '%s'\n", args ? args : "(stdin)");
}
