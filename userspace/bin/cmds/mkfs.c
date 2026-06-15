/* mkfs.c — smart dispatcher: detect fstype by name, dispatch to mkfs helpers */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkfs [-t fstype] DEVICE [BLOCK_SIZE]\n");
        printf("Supported: -t ext2, -t vfat, -t msdos, -t fat\n");
        return 1;
    }

    const char *fstype = NULL;
    const char *device = NULL;
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!device)
                device = argv[i];
        }
    }

    if (!device) {
        printf("mkfs: no device specified\n");
        printf("Usage: mkfs [-t fstype] DEVICE [BLOCK_SIZE]\n");
        return 1;
    }

    /* Auto-detect from device name or use provided fstype */
    if (!fstype) {
        const char *name = device;
        const char *slash = strrchr(device, '/');
        if (slash) name = slash + 1;

        if (strstr(name, "ext") || strstr(name, "EXT"))
            fstype = "ext2";
        else
            fstype = "vfat";  /* default to FAT */
    }

    /* Dispatch */
    char cmd[256];
    const char *fslower = fstype;

    if (strcmp(fstype, "ext2") == 0) {
        snprintf(cmd, sizeof(cmd), "/bin/mkfs_ext2 %s", device);
    } else if (strcmp(fstype, "msdos") == 0 ||
               strcmp(fstype, "vfat") == 0 ||
               strcmp(fstype, "fat") == 0 ||
               strcmp(fstype, "fat32") == 0) {
        snprintf(cmd, sizeof(cmd), "/bin/mkdosfs %s", device);
    } else {
        printf("mkfs: unsupported filesystem type '%s'\n", fstype);
        printf("Supported: ext2, vfat, msdos, fat\n");
        return 1;
    }

    printf("mkfs: creating %s filesystem on %s\n", fstype, device);

    /* Fork + exec the appropriate mkfs tool */
    int pid = fork();
    if (pid < 0) {
        printf("mkfs: fork failed\n");
        return 1;
    }

    if (pid == 0) {
        char *args[] = {(char *)cmd, (char *)device, NULL};
        char *envp[] = {NULL};
        execve(args[0], args, envp);
        /* Fallback: try relative path */
        char relpath[64];
        snprintf(relpath, sizeof(relpath), "mkfs_%s", fslower);
        args[0] = relpath;
        args[1] = (char *)device;
        args[2] = NULL;
        execve(relpath, args, envp);
        printf("mkfs: exec failed\n");
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);
    if (status != 0) {
        printf("mkfs: filesystem creation failed\n");
        return 1;
    }

    printf("mkfs: %s filesystem created on %s\n", fstype, device);
    return 0;
}
