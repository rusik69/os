/* unxz.c — XZ decompression */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (argc < 2) {
        printf("usage: unxz <file.xz>\n");
        return 1;
    }
    printf("unxz: not compiled in this build\n");
    return 0;
}
