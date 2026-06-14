/* mkfs.c — make filesystem (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Usage: mkfs [-t fstype] DEVICE [BLOCK_SIZE]\n");
    printf("mkfs: not implemented in this build\n");
    return 1;
}
