/* cmd_hostid.c — print numeric host identifier */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"

void cmd_hostid(void) {
    uint64_t id = libc_uptime_ticks() ^ (uint64_t)libc_getpid();
    kprintf("%lx\n", id);
}
