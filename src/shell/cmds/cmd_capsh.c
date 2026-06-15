/* cmd_capsh.c — capability shell */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_capsh(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("capsh: capability shell stub\n");
    return 0;
}

void capsh_init(void)
{
    kprintf("[OK] cmd_capsh: capability shell ready\n");
}