/* cmd_adjtimex.c — get/set clock adjustment */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_adjtimex(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("adjtimex: clock adjustment stub\n");
    return 0;
}

void adjtimex_init(void)
{
    kprintf("[OK] cmd_adjtimex: clock adjustment command ready\n");
}
