/* chvt.c — change virtual terminal */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* VT ioctl commands */
#define VT_ACTIVATE   0x5606
#define VT_WAITACTIVE 0x5607

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: chvt N\n");
        return 1;
    }

    int vt = atoi(argv[1]);
    if (vt < 0 || vt > 63) {
        printf("chvt: invalid terminal number %d\n", vt);
        return 1;
    }

    /* Try writing to /sys/class/vt/active */
    int fd = open("/sys/class/vt/active", O_WRONLY, 0);
    if (fd >= 0) {
        char buf[16];
        int len = snprintf(buf, sizeof(buf), "%d", vt);
        write(fd, buf, len);
        close(fd);
        return 0;
    }

    /* Try /dev/tty0 with VT_ACTIVATE + VT_WAITACTIVE */
    fd = open("/dev/tty0", O_RDONLY, 0);
    if (fd >= 0) {
        if (ioctl(fd, VT_ACTIVATE, (void *)(unsigned long)vt) == 0) {
            /* Wait for the switch to complete */
            ioctl(fd, VT_WAITACTIVE, (void *)(unsigned long)vt);
            close(fd);
            return 0;
        }
        close(fd);
    }

    /* Try /dev/tty<N> directly */
    char devpath[32];
    snprintf(devpath, sizeof(devpath), "/dev/tty%d", vt);
    fd = open(devpath, O_RDONLY, 0);
    if (fd >= 0) {
        if (ioctl(fd, VT_ACTIVATE, (void *)(unsigned long)vt) == 0) {
            ioctl(fd, VT_WAITACTIVE, (void *)(unsigned long)vt);
            close(fd);
            return 0;
        }
        close(fd);
    }

    printf("chvt: cannot switch to VT %d (no VT support)\n", vt);
    return 1;
}
