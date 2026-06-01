#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_host(const char *args) {
    (void)args;
    kprintf("host: reading '%s'\n", args ? args : "(stdin)");
}
