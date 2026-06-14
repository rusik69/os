/* md5sum.c — print stub-md5 <filename> */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("stub-md5  (stdin)\n");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) { printf("md5sum: %s: No such file\n", argv[i]); continue; }
        char buf[4096];
        unsigned long total = 0;
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) total += n;
        close(fd);
        printf("stub-md5  %s\n", argv[i]);
    }
    return 0;
}
