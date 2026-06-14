#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 3) {
        printf("Usage: chgrp GROUP FILE...\n");
        return 1;
    }
    printf("chgrp: not implemented\n");
    return 1;
}
