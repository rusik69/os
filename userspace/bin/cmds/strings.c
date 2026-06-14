#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/dev/stdin";
    int fd = open(path, 0, 0);
    if (fd < 0) { printf("strings: %s: No such file\n", path); return 1; }
    char buf[4096];
    int n;
    char str[256];
    int slen = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] >= 32 && buf[i] < 127) {
                if (slen < 255) str[slen++] = buf[i];
            } else {
                if (slen >= 4) { str[slen] = '\0'; printf("%s\n", str); }
                slen = 0;
            }
        }
    }
    if (slen >= 4) { str[slen] = '\0'; printf("%s\n", str); }
    close(fd);
    return 0;
}
