/* run.c — Run as user: just exec the command */
#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: run <command> [args...]\n");
        return 1;
    }
    /* Try to exec the command */
    execve(argv[1], argv + 1, 0);
    printf("run: cannot execute '%s'\n", argv[1]);
    return 1;
}
