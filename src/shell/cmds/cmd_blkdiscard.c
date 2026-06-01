/* cmd_blkdiscard.c — discard device sectors */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_blkdiscard(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("blkdiscard: discard sectors stub (use with caution)\n");
    return 0;
}

void blkdiscard_init(void)
{
    kprintf("[OK] cmd_blkdiscard: block discard command ready\n");
}
