/* dir.c — directory listing (like ls) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "sys/stat.h"

static void list_dir(const char *path) {
    int fd = open(path, 0, 0);
    if (fd < 0) {
        printf("dir: cannot open %s\n", path);
        return;
    }
    char buf[4096];
    long n = getdents64(fd, buf, sizeof(buf));
    if (n < 0) {
        printf("dir: error reading %s\n", path);
        close(fd);
        return;
    }
    long pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        if (d->d_name[0] != '.' || (d->d_name[1] != '\0' && !(d->d_name[1] == '.' && d->d_name[2] == '\0'))) {
            write(1, d->d_name, strlen(d->d_name));
            write(1, "  ", 2);
        }
        pos += d->d_reclen;
    }
    write(1, "\n", 1);
    close(fd);
}

int main(int argc, char *argv[]) {
    const char *path = ".";
    if (argc > 1) path = argv[1];
    list_dir(path);
    return 0;
}
