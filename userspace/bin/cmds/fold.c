#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define DEFAULT_WIDTH 80

int main(int argc, char *argv[]) {
    int width = DEFAULT_WIDTH;
    const char *path = NULL;
    int i = 1;
    if (argc > 1 && argv[1][0] == '-') {
        width = atoi(argv[1] + 1);
        i = 2;
    }
    if (argc > i) path = argv[i];
    if (!path) path = "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("fold: %s: No such file\n", path); return 1; }
    char buf[4096];
    int n;
    int col = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                write(1, "\n", 1);
                col = 0;
            } else {
                if (col >= width) {
                    write(1, "\n", 1);
                    col = 0;
                }
                write(1, &buf[i], 1);
                col++;
            }
        }
    }
    close(fd);
    return 0;
}
