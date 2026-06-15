/* rmmod.c — remove kernel module */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: rmmod MODULE\n");
        return 1;
    }

    const char *module = argv[1];

    if (delete_module(module, 0) < 0) {
        printf("rmmod: failed to remove module '%s'\n", module);
        return 1;
    }

    printf("rmmod: removed '%s'\n", module);
    return 0;
}
