/* truncate.c — shrink/extend file to a given size */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: truncate -s SIZE FILE...\n");
        return 1;
    }
    unsigned long size = 0;
    int file_start = 1;
    if (strcmp(argv[1], "-s") == 0 && argc > 2) {
        char *p = argv[2];
        while (*p >= '0' && *p <= '9') {
            size = size * 10 + (*p - '0');
            p++;
        }
        if (*p == 'K' || *p == 'k') size *= 1024;
        else if (*p == 'M' || *p == 'm') size *= 1024 * 1024;
        file_start = 3;
    }
    if (file_start >= argc) {
        printf("truncate: missing file operand\n");
        return 1;
    }
    for (int i = file_start; i < argc; i++) {
        int fd = open(argv[i], O_RDWR, 0);
        if (fd < 0) {
            printf("truncate: cannot open '%s'\n", argv[i]);
            continue;
        }
        if (ftruncate(fd, size) < 0) {
            printf("truncate: cannot truncate '%s'\n", argv[i]);
            close(fd);
            continue;
        }
        close(fd);
    }
    return 0;
}
