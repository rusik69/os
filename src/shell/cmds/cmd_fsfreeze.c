/* cmd_fsfreeze.c — freeze filesystem */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_fsfreeze(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: fsfreeze --{freeze,unfreeze} <mountpoint>\n");
        return 1;
    }

    const char *op = argv[1];
    const char *mp = argv[2];

    if (strcmp(op, "--freeze") == 0) {
        kprintf("fsfreeze: freezing '%s' (stub)\n", mp);
    } else if (strcmp(op, "--unfreeze") == 0) {
        kprintf("fsfreeze: unfreezing '%s' (stub)\n", mp);
    } else {
        kprintf("fsfreeze: unknown operation '%s'\n", op);
        return 1;
    }
    return 0;
}

void fsfreeze_init(void)
{
    kprintf("[OK] cmd_fsfreeze: filesystem freeze command ready\n");
}
