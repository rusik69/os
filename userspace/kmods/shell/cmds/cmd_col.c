#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_col(const char *args) {
    (void)args;
    kprintf("col: pass-through (stdin)\n");
}
