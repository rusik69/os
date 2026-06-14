/* cpufreq_info.c — CPU frequency info (stub) */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/cpuinfo", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
    } else {
        printf("cpufreq_info: not available\n");
    }
    return 0;
}
