/* xz.c — XZ compression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (argc < 2) {
        printf("usage: xz <file>\n");
        return 1;
    }
    printf("xz: not compiled in this build\n");
    return 0;
}
