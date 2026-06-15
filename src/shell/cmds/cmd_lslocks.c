#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_lslocks(const char *args) {
    (void)args;
    kprintf("ACTIVE LOCKS:\n  No active locks\n");
}
