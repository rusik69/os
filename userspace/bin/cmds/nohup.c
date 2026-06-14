/* nohup.c — run command immune to hangups (stub) */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("nohup: not implemented (no SIGHUP handling in this build)\n");
    return 1;
}
