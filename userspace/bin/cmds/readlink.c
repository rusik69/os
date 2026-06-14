/* readlink.c — read symlink target */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: readlink <path>\n"); return 1; }
    char buf[1024];
    int n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("readlink: %s: No such file or not a symlink\n", argv[1]);
        return 1;
    }
    buf[n] = '\0';
    printf("%s\n", buf);
    return 0;
}
