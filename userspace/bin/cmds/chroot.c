/* chroot.c — stub for chroot */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: chroot NEWROOT [COMMAND...]\n");
        return 1;
    }
    printf("chroot: not implemented (no chroot syscall in this kernel)\n");
    return 1;
}
