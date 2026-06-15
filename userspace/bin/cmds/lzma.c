/* lzma.c — LZMA compression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (argc < 2) {
        printf("usage: lzma <file>\n");
        return 1;
    }
    printf("lzma: not compiled in this build\n");
    return 0;
}
