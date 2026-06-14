#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: xz [file]\n");
        return 1;
    }
    printf("xz: not implemented\n");
    return 1;
}
