#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
void cmd_arch(const char *args) { (void)args; kprintf("x86_64\n"); }
