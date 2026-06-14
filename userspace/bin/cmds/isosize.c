/* isosize.c — show ISO filesystem size (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: isosize <iso-file>\n");
        return 1;
    }
    printf("isosize: not yet implemented\n");
    return 1;
}
