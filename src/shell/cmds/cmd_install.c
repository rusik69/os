#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_install(const char *args) {
    (void)args;
    kprintf("install: reading '%s'\n", args ? args : "(stdin)");
}
