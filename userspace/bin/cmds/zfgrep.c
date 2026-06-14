#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: zfgrep PATTERN [FILE...]\n");
        return 1;
    }
    printf("zfgrep: not implemented\n");
    return 1;
}
