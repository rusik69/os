/* losetup.c — set up/control loop devices (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Usage: losetup [OPTION]... DEVICE FILE\n");
    printf("losetup: not implemented (no loop device support in this build)\n");
    return 1;
}
