/* swapon.c — enable swap device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: swapon DEVICE\n");
        return 1;
    }

    const char *device = argv[1];

    if (swapon(device) < 0) {
        printf("swapon: failed to enable swap on '%s'\n", device);
        return 1;
    }

    printf("swapon: swap enabled on '%s'\n", device);
    return 0;
}
