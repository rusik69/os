/* install.c — copy files and set permissions */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 3) { printf("Usage: install <src> <dst>\n"); return 1; }
    const char *src = argv[1];
    const char *dst = argv[2];
    int in_fd = open(src, O_RDONLY, 0);
    if (in_fd < 0) { printf("install: %s: No such file\n", src); return 1; }
    int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (out_fd < 0) { printf("install: cannot create '%s'\n", dst); close(in_fd); return 1; }
    char buf[4096];
    int n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0)
        write(out_fd, buf, n);
    close(in_fd);
    close(out_fd);
    return 0;
}
