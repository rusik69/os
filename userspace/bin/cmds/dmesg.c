/* dmesg.c — read kernel log from /dev/kmsg or /proc/kmsg */

#include "unistd.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Try /dev/kmsg first, then /proc/kmsg */
    int fd = open("/dev/kmsg", O_RDONLY, 0);
    if (fd < 0) {
        fd = open("/proc/kmsg", O_RDONLY, 0);
    }
    if (fd < 0) {
        printf("dmesg: no kernel log available\n");
        return 1;
    }

    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        write(1, buf, n);
    }
    close(fd);
    return 0;
}
