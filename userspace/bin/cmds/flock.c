/* flock.c — file lock (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: flock <file> <cmd> [args...]\n");
        return 1;
    }
    printf("flock: not yet implemented\n");
    return 1;
}
