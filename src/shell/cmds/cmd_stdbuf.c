#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_stdbuf(const char *args) {
    (void)args;
    kprintf("stdbuf: reading '%s'\n", args ? args : "(stdin)");
}
