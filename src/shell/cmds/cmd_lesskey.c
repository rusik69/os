#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lesskey(const char *args) {
    (void)args;
    kprintf("lesskey: reading '%s'\n", args ? args : "(stdin)");
}
