#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_who(const char *args) {
    (void)args;
    kprintf("root     ttyS0    Jun 15 17:08\n");
}
