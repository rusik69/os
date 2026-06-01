#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_isosize(const char *args) {
    (void)args;
    kprintf("isosize: reading '%s'\n", args ? args : "(stdin)");
}
