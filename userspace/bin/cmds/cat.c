/* cat.c — read files and print to stdout */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* Read from stdin */
        char buf[512];
        int n;
        while ((n = read(0, buf, 512)) > 0) {
            write(1, buf, n);
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            printf("cat: cannot open '%s'\n", argv[i]);
            continue;
        }
        char buf[512];
        int n;
        while ((n = read(fd, buf, 512)) > 0) {
            write(1, buf, n);
        }
        close(fd);
    }
    return 0;
}
