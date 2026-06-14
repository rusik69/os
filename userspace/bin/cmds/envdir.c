/* envdir.c — run command with modified environment (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: envdir <dir> <cmd> [args...]\n");
        return 1;
    }
    printf("envdir: not yet implemented\n");
    return 1;
}
