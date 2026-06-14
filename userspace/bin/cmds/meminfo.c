/* meminfo.c — Memory info: read /proc/meminfo */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    char buf[4096];
    int fd = open("/proc/meminfo", O_RDONLY, 0);
    if (fd < 0) {
        printf("meminfo: /proc/meminfo not available\n");
        return 1;
    }
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
