/* renice.c — change priority of running processes (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("renice: not implemented (no setpriority/getpriority syscall wrappers)\n");
    return 1;
}
