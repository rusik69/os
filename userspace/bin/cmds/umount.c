/* umount.c — unmount filesystem */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: umount [-f] MOUNTPOINT|DEVICE\n");
        return 1;
    }

    const char *target = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            target = argv[i];
            break;
        }
    }

    if (!target) {
        printf("Usage: umount [-f] MOUNTPOINT|DEVICE\n");
        return 1;
    }

    if (umount(target) < 0) {
        printf("umount: failed to unmount '%s'\n", target);
        return 1;
    }

    printf("umount: unmounted '%s'\n", target);
    return 0;
}
