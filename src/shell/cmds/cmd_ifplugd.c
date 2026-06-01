/* cmd_ifplugd.c — link monitor stub */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_ifplugd(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("ifplugd: link beat monitor (stub — not running as daemon)\n");
    kprintf("  eth0: link detected at 1000Mbps (simulated)\n");
    return 0;
}

void ifplugd_init(void)
{
    kprintf("[OK] cmd_ifplugd: link monitor ready\n");
}
