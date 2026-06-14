/* getopt.c — parse command options (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: getopt <optstring> [args...]\n");
        return 1;
    }
    printf("getopt: not yet implemented\n");
    return 1;
}
