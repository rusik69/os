/* setsid.c — start new session and run command (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("setsid: not implemented (no setsid syscall in this build)\n");
    return 1;
}
