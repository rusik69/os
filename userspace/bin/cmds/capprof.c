/* capprof.c — set syscall profile (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("capprof: not supported\n");
    return 1;
}
