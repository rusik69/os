/* swapoff.c — disable swap device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    int i, any = 0;

    if (argc < 2) {
        printf("Usage: swapoff [DEVICE ...]\n");
        return 1;
    }

    for (i = 1; i < argc; i++) {
        const char *device = argv[i];
        if (swapoff(device) < 0) {
            printf("swapoff: failed to disable swap on '%s'\n", device);
            return 1;
        }
        printf("swapoff: swap disabled on '%s'\n", device);
        any = 1;
    }

    return any ? 0 : 1;
}