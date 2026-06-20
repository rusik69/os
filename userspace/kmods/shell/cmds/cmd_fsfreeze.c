/* cmd_fsfreeze.c — freeze filesystem */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "freeze.h"

int cmd_fsfreeze(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: fsfreeze --{freeze,unfreeze} <mountpoint>\n");
        return 1;
    }

    const char *op = argv[1];
    const char *mp = argv[2];

    (void)mp;  /* mountpoint is for future per-fs support; currently
                * freeze/thaw operates on all writable filesystems */

    if (strcmp(op, "--freeze") == 0) {
        int ret = freeze_fs();
        if (ret < 0) {
            kprintf("fsfreeze: failed to freeze filesystems: %d\n", ret);
            return 1;
        }
        kprintf("fsfreeze: filesystem(s) frozen\n");
    } else if (strcmp(op, "--unfreeze") == 0) {
        int ret = thaw_fs();
        if (ret < 0) {
            kprintf("fsfreeze: failed to unfreeze filesystems: %d\n", ret);
            return 1;
        }
        kprintf("fsfreeze: filesystem(s) unfrozen\n");
    } else {
        kprintf("fsfreeze: unknown operation '%s'\n", op);
        kprintf("usage: fsfreeze --{freeze,unfreeze} <mountpoint>\n");
        return 1;
    }
    return 0;
}

void fsfreeze_init(void)
{
    kprintf("[OK] cmd_fsfreeze: filesystem freeze command ready\n");
}
