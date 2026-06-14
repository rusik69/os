/* ifconfig.c — interface config: read /proc/net/dev */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/net/dev", O_RDONLY, 0);
    if (fd < 0) {
        printf("ifconfig: cannot open /proc/net/dev\n");
        return 1;
    }
    char buf[2048];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        write(1, buf, n);
    }
    close(fd);
    return 0;
}
