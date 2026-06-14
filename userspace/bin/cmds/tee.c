/* tee.c — read from stdin and write to stdout and files */

#include "unistd.h"
#include "stdio.h"

#define MAX_FILES 16
#define BUF_SIZE 4096

int main(int argc, char *argv[]) {
    int files[MAX_FILES];
    int nfiles = 0;

    for (int i = 1; i < argc && nfiles < MAX_FILES; i++) {
        files[nfiles] = open(argv[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (files[nfiles] < 0) {
            printf("tee: cannot open '%s'\n", argv[i]);
        } else {
            nfiles++;
        }
    }

    char buf[BUF_SIZE];
    int nread;
    while ((nread = read(0, buf, BUF_SIZE)) > 0) {
        write(1, buf, nread);
        for (int i = 0; i < nfiles; i++) {
            write(files[i], buf, nread);
        }
    }

    for (int i = 0; i < nfiles; i++) close(files[i]);
    return 0;
}
