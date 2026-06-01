#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_sha1sum(const char *args) {
    (void)args;
    kprintf("sha1sum: reading '%s'\n", args ? args : "(stdin)");
}
