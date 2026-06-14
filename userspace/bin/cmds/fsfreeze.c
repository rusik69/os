/* fsfreeze.c — freeze/unfreeze filesystem (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: fsfreeze --freeze|--unfreeze <mountpoint>\n");
        return 1;
    }
    (void)argv;
    printf("fsfreeze: not supported\n");
    return 1;
}
