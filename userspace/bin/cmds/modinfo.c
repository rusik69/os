#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("Usage: modinfo MODULE\n");
        return 1;
    }
    /* Try reading /proc/modules */
    int fd = open("/proc/modules", 0, 0);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            printf("%s", buf);
            return 0;
        }
    }
    printf("modinfo: not implemented\n");
    return 1;
}
