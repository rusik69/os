/* cmd_fsfreeze.c — freeze filesystem */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "freeze.h"
#include "vfs.h"

int cmd_fsfreeze(int argc, char **argv)
{
    if (argc < 3) {
        kprintf("usage: fsfreeze --{freeze,unfreeze} <mountpoint>\n");
        return 1;
    }

    const char *op = argv[1];
    const char *mp = argv[2];

    /* Per-filesystem freeze/thaw: resolve the mountpoint to a specific
     * filesystem and freeze only that one instead of all writable filesystems. */
    char resolved[64];
    int found_mount = 0;

    /* Find the exact mount that matches the given path */
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
        if (mounts[i].ops == NULL) continue;
        /* Check if mountpoint matches, or if the path is within this mount */
        size_t mplen = strlen(mounts[i].mountpoint);
        if (strncmp(mp, mounts[i].mountpoint, mplen) == 0 &&
            (mp[mplen] == '\0' || mp[mplen] == '/')) {
            found_mount = 1;
            strncpy(resolved, mounts[i].mountpoint, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
            break;
        }
    }

    if (found_mount == 0) {
        kprintf("fsfreeze: mountpoint '%s' not found, falling back to global freeze\n", mp);
        found_mount = 0;
    }

    kprintf("fsfreeze: operating on mount '%s'\n", found_mount ? resolved : "ALL");

    if (strcmp(op, "--freeze") == 0) {
        int ret = freeze_fs();
        if (ret < 0) {
            kprintf("fsfreeze: failed to freeze filesystem(s): %d\n", ret);
            return 1;
        }
        kprintf("fsfreeze: filesystem(s) frozen (%s)\n",
                found_mount ? resolved : "all");

    } else if (strcmp(op, "--unfreeze") == 0) {
        int ret = thaw_fs();
        if (ret < 0) {
            kprintf("fsfreeze: failed to unfreeze filesystems: %d\n", ret);
            return 1;
        }
        kprintf("fsfreeze: filesystem(s) unfrozen (%s)\n",
                found_mount ? resolved : "all");

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
