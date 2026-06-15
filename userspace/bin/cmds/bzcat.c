/* bzcat.c — Decompress bzip2 to stdout */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (argc < 2) {
        printf("usage: bzcat <file.bz2>\n");
        return 1;
    }
    printf("bzcat: not compiled in this build (use gzip instead)\n");
    return 0;
}
