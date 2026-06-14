/* sha256sum.c — print stub-sha256 <filename> */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("stub-sha256  (stdin)\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf("sha256sum: %s: No such file\n", argv[i]); continue; }
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0);
        close(fd);
        printf("stub-sha256  %s\n", argv[i]);
    }
    return 0;
}
