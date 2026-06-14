/* devmem2.c — enhanced devmem (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("devmem2: not supported (requires /dev/mem)\n");
    return 1;
}
