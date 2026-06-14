/* uptime.c — read /proc/uptime */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/uptime", O_RDONLY, 0);
    if (fd < 0) {
        printf("uptime: cannot open /proc/uptime\n");
        return 1;
    }
    char buf[128];
    int n = read(fd, buf, 127);
    close(fd);
    if (n <= 0) {
        printf("uptime: read error\n");
        return 1;
    }
    buf[n] = '\0';
    /* Remove trailing newline */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
    printf("up %s seconds\n", buf);
    return 0;
}
