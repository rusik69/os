/* cmd_getopt.c — parse options */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_getopt(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    kprintf("getopt: option parser stub\n");
    kprintf("usage: getopt <optstring> <parameters>\n");
    return 0;
}

void getopt_init(void)
{
    kprintf("[OK] cmd_getopt: option parser ready\n");
}