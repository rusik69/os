/* insmod.c — insert module (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: insmod <module.ko>\n");
        return 1;
    }
    (void)argv;
    printf("insmod: not supported (requires kernel module support)\n");
    return 1;
}
