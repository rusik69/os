/* cmd_ss.c — Socket statistics (show network connections) */
#include "shell_cmds.h"
#include "printf.h"
#include "libc.h"

void cmd_ss(const char *args) {
    (void)args;
    kprintf("Active network connections:\n");
    kprintf("Proto Recv-Q Send-Q Local Address           Foreign Address         State\n");
    /* Use the netstat syscall to list connections */
    libc_netstat();
}
