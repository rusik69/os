#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha512sum(const char *args) {
    (void)args;
    kprintf("sha512sum: reading '%s'\n", args ? args : "(stdin)");
}
