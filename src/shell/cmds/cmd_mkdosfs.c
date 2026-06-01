/* cmd_mkdosfs.c — create FAT filesystem stub */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_mkdosfs(int argc, char **argv)
{
    const char *device = NULL;
    int fat_size = 32;  /* default FAT32 */

    if (argc < 2) {
        kprintf("usage: mkdosfs [-F FAT-size] <device> [block-count]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-F") == 0 && i + 1 < argc) {
            fat_size = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            device = argv[i];
        }
    }

    if (!device) {
        kprintf("mkdosfs: no device specified\n");
        return 1;
    }

    kprintf("mkdosfs: creating FAT%d filesystem on '%s' (stub)\n",
            fat_size, device);
    return 0;
}

void mkdosfs_init(void)
{
    kprintf("[OK] cmd_mkdosfs: FAT filesystem creator ready\n");
}
