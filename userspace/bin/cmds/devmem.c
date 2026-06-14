/* devmem.c — physical memory read/write (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("devmem: not supported (requires /dev/mem)\n");
    return 1;
}
