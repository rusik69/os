#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    char hostname[256];
    if (argc > 1) {
        /* set hostname (requires root, just try) */
        int fd = open("/proc/hostname", 1, 0);
        if (fd >= 0) {
            write(fd, argv[1], strlen(argv[1]));
            close(fd);
            return 0;
        }
        /* fallback to syscall */
        hostname[0] = '\0';
        if (gethostname(hostname, sizeof(hostname)) < 0) {
            printf("hostname: error\n");
            return 1;
        }
        return 0;
    }
    /* get hostname */
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        printf("hostname: error\n");
        return 1;
    }
    hostname[sizeof(hostname)-1] = '\0';
    printf("%s\n", hostname);
    return 0;
}
