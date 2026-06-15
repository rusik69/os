#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_column(const char *args) {
    (void)args;
    kprintf("column: reading stdin\n");
}
