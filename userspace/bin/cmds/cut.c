#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    char delim = '\t';
    int fields = -1;
    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if (argv[i][1] == 'd' && argv[i][2]) { delim = argv[i][2]; i++; }
        else if (argv[i][1] == 'd' && i+1 < argc) { delim = argv[i+1][0]; i += 2; }
        else if (argv[i][1] == 'f' && argv[i][2]) { fields = atoi(argv[i]+2); i++; }
        else if (argv[i][1] == 'f' && i+1 < argc) { fields = atoi(argv[i+1]); i += 2; }
        else break;
    }
    if (fields < 0) { printf("Usage: cut -d<delim> -f<field> [file]\n"); return 1; }
    const char *path = (i < argc) ? argv[i] : "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("cut: %s: No such file\n", path); return 1; }
    char buf[4096];
    int n;
    int f = 0;
    int printed = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int j = 0; j < n; j++) {
            if (buf[j] == '\n') {
                if (f+1 == fields && printed) write(1, "\n", 1);
                else if (printed) write(1, "\n", 1);
                f = 0;
                printed = 0;
            } else if (buf[j] == delim) {
                f++;
            } else {
                if (f+1 == fields) {
                    write(1, &buf[j], 1);
                    printed = 1;
                }
            }
        }
    }
    close(fd);
    return 0;
}
