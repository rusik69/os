/* bunzip2.c — Decompress bzip2 file */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("bunzip2: not compiled in this build (use gzip instead)\n");
    return 0;
}
