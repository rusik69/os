/* cc.c — C compiler (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("cc: not available in userspace\n");
    return 1;
}
