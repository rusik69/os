/* lsmod.c — List modules: read /proc/modules */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    char buf[4096];
    int fd = open("/proc/modules", O_RDONLY, 0);
    if (fd < 0) {
        printf("lsmod: /proc/modules not available\n");
        return 1;
    }
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
