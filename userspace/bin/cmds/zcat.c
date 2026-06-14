#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    char buf[4096];
    int n;
    if (argc > 1) {
        int fd = open(argv[1], 0, 0);
        if (fd < 0) {
            printf("zcat: cannot open %s\n", argv[1]);
            return 1;
        }
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    while ((n = read(0, buf, sizeof(buf))) > 0) {
        write(1, buf, n);
    }
    return 0;
}
