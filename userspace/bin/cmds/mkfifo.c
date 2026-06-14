/* mkfifo.c — create FIFO (named pipe) (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("mkfifo: not implemented (no mkfifo/mknod syscall in this build)\n");
    return 1;
}
