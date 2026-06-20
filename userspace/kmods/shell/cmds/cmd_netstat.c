#include "shell.h"
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
void cmd_netstat(const char *args) {
    (void)args;
    kprintf("Active connections:\n");
    kprintf("  (kernel netstat not available)\n");
}
