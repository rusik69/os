/* cmd_hostid.c — print the host/machine ID */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_hostid(const char *args) {
    (void)args;
    /* Return a simple host ID based on uptime ticks and PID */
    uint32_t id = (uint32_t)(libc_uptime_ticks() ^ libc_getpid());
    kprintf("%08x\n", (uint64_t)id);
}
