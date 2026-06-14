/* sha512sum.c — SHA-512 checksum stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("sha512-unavailable  (stdin)\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf("sha512sum: %s: No such file\n", argv[i]); continue; }
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0);
        close(fd);
        printf("sha512-unavailable  %s\n", argv[i]);
    }
    return 0;
}
