/* cmp.c — compare two files byte by byte */

#include "unistd.h"
#include "stdio.h"

#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("Usage: cmp <file1> <file2>\n");
        return 1;
    }
    int fd1 = open(argv[1], O_RDONLY, 0);
    int fd2 = open(argv[2], O_RDONLY, 0);
    if (fd1 < 0 || fd2 < 0) {
        printf("cmp: cannot open files\n");
        if (fd1 >= 0) close(fd1);
        if (fd2 >= 0) close(fd2);
        return 1;
    }
    char buf1[BUF_SIZE], buf2[BUF_SIZE];
    unsigned long long offset = 0;
    int n1, n2;
    while ((n1 = read(fd1, buf1, BUF_SIZE)) > 0 &&
           (n2 = read(fd2, buf2, BUF_SIZE)) > 0) {
        int min = n1 < n2 ? n1 : n2;
        for (int i = 0; i < min; i++) {
            if (buf1[i] != buf2[i]) {
                printf("cmp: %s %s differ: byte %llu\n", argv[1], argv[2], offset + i + 1);
                close(fd1);
                close(fd2);
                return 1;
            }
        }
        if (n1 != n2) {
            printf("cmp: EOF on %s after %llu bytes\n",
                   n1 < n2 ? argv[1] : argv[2], offset + min);
            close(fd1);
            close(fd2);
            return 1;
        }
        offset += min;
    }
    if (n1 != n2) {
        printf("cmp: EOF on %s after %llu bytes\n",
               n1 < n2 ? argv[1] : argv[2], offset);
        close(fd1);
        close(fd2);
        return 1;
    }
    close(fd1);
    close(fd2);
    return 0;
}
