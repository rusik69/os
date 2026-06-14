#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void) {
    /* Try reading /proc/net/tcp */
    int fd = open("/proc/net/tcp", 0, 0);
    if (fd >= 0) {
        char buf[2048];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            printf("%s", buf);
            return 0;
        }
    }
    printf("netstat: not implemented\n");
    return 1;
}
