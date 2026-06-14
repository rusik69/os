#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 3) {
        printf("Usage: zcmp FILE1 FILE2\n");
        return 1;
    }
    printf("zcmp: not implemented\n");
    return 1;
}
