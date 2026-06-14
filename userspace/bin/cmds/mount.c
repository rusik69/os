/* mount.c — mount filesystem (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Usage: mount [-t fstype] DEVICE MOUNTPOINT\n");
    printf("mount: not implemented (no mount syscall wrapper in libc)\n");
    return 1;
}
