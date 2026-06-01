#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_timeout(const char *args) {
    (void)args;
    kprintf("timeout: reading '%s'\n", args ? args : "(stdin)");
}
