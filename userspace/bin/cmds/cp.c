/* cp.c — copy files */

#include "unistd.h"
#include "stdio.h"

#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: cp <src> <dst>\n");
        return 1;
    }
    int fd_src = open(argv[1], O_RDONLY, 0);
    if (fd_src < 0) {
        printf("cp: cannot open '%s'\n", argv[1]);
        return 1;
    }
    int fd_dst = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_dst < 0) {
        printf("cp: cannot create '%s'\n", argv[2]);
        close(fd_src);
        return 1;
    }
    char buf[BUF_SIZE];
    int n;
    while ((n = read(fd_src, buf, BUF_SIZE)) > 0) {
        if (write(fd_dst, buf, n) != n) {
            printf("cp: write error\n");
            close(fd_src);
            close(fd_dst);
            return 1;
        }
    }
    close(fd_src);
    close(fd_dst);
    return 0;
}
