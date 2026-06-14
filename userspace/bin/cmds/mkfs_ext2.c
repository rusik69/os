#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: mkfs_ext2 DEVICE\n");
        return 1;
    }
    printf("mkfs_ext2: not implemented\n");
    return 1;
}
