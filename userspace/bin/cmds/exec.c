/* exec.c — execute command */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: exec <cmd> [args...]\n");
        return 1;
    }
    /* environ is not available in this environment, pass NULL */
    execve(argv[1], &argv[1], 0);
    /* If we get here, execve failed */
    printf("exec: %s: not found\n", argv[1]);
    return 1;
}
