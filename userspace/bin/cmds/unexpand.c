#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define DEFAULT_TAB 8

int main(int argc, char *argv[]) {
    int tabstop = DEFAULT_TAB;
    const char *path = NULL;
    int i = 1;
    if (argc > 1 && argv[1][0] == '-') {
        tabstop = atoi(argv[1] + 1);
        i = 2;
    }
    if (argc > i) path = argv[i];
    if (!path) path = "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("unexpand: %s: No such file\n", path); return 1; }
    char buf[4096];
    int n;
    int col = 0;
    int spc = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == ' ') {
                spc++;
                if (col + spc == tabstop) {
                    write(1, "\t", 1);
                    col = 0;
                    spc = 0;
                }
            } else {
                while (spc > 0) { write(1, " ", 1); spc--; col++; }
                write(1, &buf[i], 1);
                if (buf[i] == '\n') { col = 0; spc = 0; }
                else col++;
            }
        }
    }
    while (spc > 0) { write(1, " ", 1); spc--; col++; }
    close(fd);
    return 0;
}
