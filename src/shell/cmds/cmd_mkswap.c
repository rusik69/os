/* cmd_mkswap.c — setup swap stub */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_mkswap(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("usage: mkswap <device> [size-in-blocks]\n");
        return 1;
    }

    const char *device = argv[1];
    uint32_t blocks = 0;

    if (argc >= 3)
        blocks = (uint32_t)strtoul(argv[2], NULL, 10);

    kprintf("mkswap: setting up swap space on '%s'", device);
    if (blocks > 0)
        kprintf(" (%u blocks)", blocks);
    kprintf(" (stub)\n");
    return 0;
}

void mkswap_init(void)
{
    kprintf("[OK] cmd_mkswap: swap setup command ready\n");
}
