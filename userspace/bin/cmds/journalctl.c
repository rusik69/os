/* journalctl.c — query the kernel journal */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(void) {
    /* Try reading kernel log directly via /proc/kmsg or dmesg interface */
    int fd = open("/proc/kmsg", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }

    fd = open("/dev/kmsg", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }

    printf("journalctl: use kernel shell 'journalctl' command\n");
    return 0;
}
