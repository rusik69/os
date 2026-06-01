#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_base32(const char *args) {
    (void)args;
    kprintf("base32: reading '%s'\n", args ? args : "(stdin)");
}
