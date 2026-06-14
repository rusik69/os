/* perf.c — Performance monitoring stub */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    /* Try reading /proc/stat */
    (void)argc;
    (void)argv;
    char buf[4096];
    int fd = open("/proc/stat", O_RDONLY, 0);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    printf("perf: not available\n");
    return 0;
}
