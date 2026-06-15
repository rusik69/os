/* eject.c — eject removable media */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* CDROMEJECT ioctl command (Linux-compatible) */
#define CDROMEJECT 0x5309
int main(int argc, char *argv[]) {
    int opt_t = 0;  /* close tray */
    const char *device = "/dev/cdrom";  /* default */
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            /* -r: eject media (default behavior) */
        } else if (strcmp(argv[i], "-s") == 0) {
            /* -s: eject slot (not implemented) */
        } else if (strcmp(argv[i], "-t") == 0) {
            opt_t = 1;
        } else if (argv[i][0] == '-') {
            printf("usage: eject [-r] [-s] [-t] [device]\n");
            return 1;
        } else {
            device = argv[i];
        }
    }

    int fd = open(device, O_RDONLY, 0);
    if (fd < 0) {
        printf("eject: cannot open '%s'\n", device);
        return 1;
    }

    if (opt_t) {
        /* Close tray — try CDROMCLOSETRAY */
        /* If not available, just report */
        int ret = ioctl(fd, 0x5319, 0); /* CDROMCLOSETRAY = 0x5319 */
        if (ret < 0) {
            printf("eject: cannot close tray (ioctl not supported)\n");
            close(fd);
            return 1;
        }
        printf("eject: closed tray on '%s'\n", device);
    } else {
        /* Eject media */
        int ret = ioctl(fd, CDROMEJECT, 0);
        if (ret < 0) {
            printf("eject: cannot eject '%s' (ioctl not supported or no media)\n", device);
            close(fd);
            return 1;
        }
        printf("eject: ejected '%s'\n", device);
    }

    close(fd);
    return 0;
}
