/* head.c — output the first part of files */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define DEFAULT_LINES 10

static void head_file(const char *path, int n) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("head: cannot open '%s'\n", path);
        return;
    }
    char buf[512];
    int lines = 0;
    int pos = 0;
    int nread;
    while ((nread = read(fd, buf + pos, 512 - pos)) > 0) {
        nread += pos;
        int start = 0;
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                buf[i] = '\0';
                printf("%s\n", buf + start);
                lines++;
                start = i + 1;
                if (lines >= n) goto done;
            }
        }
        if (start < nread) {
            int rem = nread - start;
            /* Move remaining partial line to front */
            for (int i = 0; i < rem; i++) buf[i] = buf[start + i];
            pos = rem;
        } else {
            pos = 0;
        }
    }
    if (pos > 0) {
        buf[pos] = '\0';
        printf("%s\n", buf);
    }
done:
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
        /* Read from stdin */
        char buf[512];
        int lines = 0;
        int nread;
        while ((nread = read(0, buf, 512)) > 0) {
            for (int i = 0; i < nread; i++) {
                if (buf[i] == '\n') {
                    write(1, "\n", 1);
                    lines++;
                    if (lines >= n) return 0;
                } else {
                    write(1, &buf[i], 1);
                }
            }
        }
        return 0;
    }
    for (int i = arg_start; i < argc; i++) {
        head_file(argv[i], n);
    }
    return 0;
}
