/* cmd_depmod.c — module dependencies stub */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_depmod(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("depmod: module dependency analysis stub\n");
    return 0;
}

void depmod_init(void)
{
    kprintf("[OK] cmd_depmod: module dependencies ready\n");
}
