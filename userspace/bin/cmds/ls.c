/* ls.c — list directory contents using getdents64 */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : ".";
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }
    char buf[BUF_SIZE];
    int n = getdents64(fd, buf, BUF_SIZE);
    close(fd);
    if (n < 0) {
        printf("ls: getdents failed\n");
        return 1;
    }
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        printf("%s  ", d->d_name);
        pos += d->d_reclen;
    }
    printf("\n");
    return 0;
}
