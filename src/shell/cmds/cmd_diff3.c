#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_diff3(const char *args) {
    (void)args;
    kprintf("diff3: reading '%s'\n", args ? args : "(stdin)");
}
