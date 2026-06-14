/* mknod.c — create block/char device node (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("mknod: not implemented (no mknod syscall in this build)\n");
    return 1;
}
