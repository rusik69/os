#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_ptx(const char *args) {
    (void)args;
    kprintf("ptx: reading '%s'\n", args ? args : "(stdin)");
}
