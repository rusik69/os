/* cmd_fbset.c — framebuffer settings */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_fbset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("fbset: framebuffer settings stub\n");
    kprintf("  Current mode: 1024x768-32bpp@60Hz (simulated)\n");
    return 0;
}

void fbset_init(void)
{
    kprintf("[OK] cmd_fbset: framebuffer settings ready\n");
}
