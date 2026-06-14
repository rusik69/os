#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("nl: %s: No such file\n", path); return 1; }
    char buf[4096];
    int n;
    unsigned long lineno = 1;
    int at_start = 1;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (at_start) {
                printf("%6lu\t", lineno++);
                at_start = 0;
            }
            write(1, &buf[i], 1);
            if (buf[i] == '\n') at_start = 1;
        }
    }
    close(fd);
    return 0;
}
