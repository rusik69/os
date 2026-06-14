/* umount.c — unmount filesystem (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Usage: umount [-f] MOUNTPOINT|DEVICE\n");
    printf("umount: not implemented (no umount syscall wrapper in libc)\n");
    return 1;
}
