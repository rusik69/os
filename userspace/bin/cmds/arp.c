#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc > 1) {
        printf("Usage: arp\n");
        return 1;
    }
    int fd = open("/proc/net/arp", 0, 0);
    if (fd >= 0) {
        char buf[1024];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            printf("%s", buf);
            return 0;
        }
    }
    printf("arp: not implemented\n");
    return 1;
}
