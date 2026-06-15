/* mount.c — mount filesystem */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: mount [-t fstype] DEVICE MOUNTPOINT\n");
        return 1;
    }

    const char *fstype = NULL;
    const char *device = NULL;
    const char *mountpoint = NULL;
    unsigned long flags = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0) {
            flags |= 1;  /* MS_RDONLY */
        } else if (strcmp(argv[i], "--bind") == 0 || strcmp(argv[i], "-B") == 0) {
            flags |= 0x40;  /* MS_BIND */
        } else if (argv[i][0] != '-') {
            if (!device)
                device = argv[i];
            else if (!mountpoint)
                mountpoint = argv[i];
        }
    }

    if (!device || !mountpoint) {
        printf("Usage: mount [-t fstype] DEVICE MOUNTPOINT\n");
        return 1;
    }

    if (mount(device, mountpoint, fstype ? fstype : "", flags, NULL) < 0) {
        printf("mount: failed to mount '%s' at '%s'", device, mountpoint);
        if (fstype) printf(" (type %s)", fstype);
        printf("\n");
        return 1;
    }

    printf("mount: mounted '%s' at '%s'", device, mountpoint);
    if (fstype) printf(" (type %s)", fstype);
    printf("\n");
    return 0;
}
