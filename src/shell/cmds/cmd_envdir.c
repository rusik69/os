/* cmd_envdir.c — run with env */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_envdir(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: envdir <directory> <command> [args...]\n");
        return 1;
    }

    kprintf("envdir: running '%s' with env from '%s' (stub)\n",
            argv[2], argv[1]);
    return 0;
}

void envdir_init(void)
{
    kprintf("[OK] cmd_envdir: environment directory runner ready\n");
}
