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

/* Read from stdin, printing last n lines */
static void tail_stdin(int n) {
    char buf[4096];
    int nread;
    int total = 0;
    long pos[4096]; /* track newline positions */
    int npos = 0;

    pos[npos++] = 0; /* start of data */

    while ((nread = read(0, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < nread; i++) {
            if (buf[i] == '\n') {
                if (npos < 4096)
                    pos[npos++] = total + i + 1;
            }
        }
        total += nread;
    }

    /* Determine start position for last n lines */
    int skip = npos - n;
    if (skip < 0) skip = 0;
    long start = pos[skip];

    /* Re-read from stdin from the start */
    lseek(0, start, SEEK_SET);

    /* Read and print remaining */
    char out[4096];
    int nout;
    while ((nout = read(0, out, sizeof(out))) > 0) {
        write(1, out, nout);
    }
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
        /* Stdin mode */
        tail_stdin(n);
        return 0;
    }
    for (int i = arg_start; i < argc; i++) {
        tail_file(argv[i], n);
    }
    return 0;
}
