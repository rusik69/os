/* tail.c — output the last part of files */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define DEFAULT_LINES 10

static void tail_file(const char *path, int n) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("tail: cannot open '%s'\n", path);
        return;
    }
    char buf[4096];
    int nread;

    /* Count total lines */
    int total = 0;
    while ((nread = read(fd, buf, 4096)) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') total++;
        }
    }

    /* Rewind */
    close(fd);
    fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;

    /* Skip lines we don't want */
    int skip = total - n;
    if (skip < 0) skip = 0;
    int count = 0;
    while (count < skip && (nread = read(fd, buf, 1)) == 1) {
        if (buf[0] == '\n') count++;
    }

    /* Print remaining */
    while ((nread = read(fd, buf, 4096)) > 0) {
        write(1, buf, nread);
    }
    close(fd);
}

int main(int argc, char *argv[]) {
    int n = DEFAULT_LINES;
    int arg_start = 1;
    if (argc > 1 && argv[1][0] == '-') {
        n = 0;
        for (int i = 1; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
            n = n * 10 + (argv[1][i] - '0');
        arg_start = 2;
    }
    if (argc <= arg_start) {
        printf("tail: stdin not supported\n");
        return 1;
    }
    for (int i = arg_start; i < argc; i++) {
        tail_file(argv[i], n);
    }
    return 0;
}
