#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha224sum(const char *args) {
    (void)args;
    kprintf("sha224sum: reading '%s'\n", args ? args : "(stdin)");
}
