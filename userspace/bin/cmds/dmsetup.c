/* dmsetup.c — device mapper (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("dmsetup: not supported\n");
    return 1;
}
