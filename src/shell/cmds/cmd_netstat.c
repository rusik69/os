/* cmd_netstat.c — show network connections and listening ports */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

void cmd_netstat(const char *args) {
    (void)args;
    libc_netstat();
}
