/* unlzma.c — LZMA decompression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (argc < 2) {
        printf("usage: unlzma <file.lzma>\n");
        return 1;
    }
    printf("unlzma: not compiled in this build\n");
    return 0;
}
