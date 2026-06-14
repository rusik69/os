/* cpuinfo.c — show CPU info */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to read /proc/cpuinfo */
    int fd = open("/proc/cpuinfo", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    printf("cpuinfo: /proc/cpuinfo not available\n");
    return 1;
}
