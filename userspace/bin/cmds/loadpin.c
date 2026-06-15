/* loadpin.c — Load pinning status */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    int verbose = 0;
    if (argc > 1 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
    } else if (argc > 1) {
        printf("usage: loadpin [-v]\n");
        return 1;
    }

    int fd = open("/sys/kernel/security/loadpin/enabled", O_RDONLY, 0);
    if (fd < 0) {
        printf("loadpin: not enabled or /sys not mounted\n");
        return 1;
    }
    char buf[64];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = 0;
        /* Trim newline */
        if (n > 0 && buf[n-1] == '\n') buf[n-1] = 0;
        printf("LoadPin: %s\n", buf);
    }

    if (verbose) {
        fd = open("/sys/kernel/security/loadpin/dm_verity", O_RDONLY, 0);
        if (fd >= 0) {
            n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = 0;
                if (n > 0 && buf[n-1] == '\n') buf[n-1] = 0;
                printf("dm-verity: %s\n", buf);
            }
        } else {
            printf("dm-verity: not available\n");
        }
    }

    return 0;
}
