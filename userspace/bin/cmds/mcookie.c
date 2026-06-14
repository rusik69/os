#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void) {
    unsigned char buf[16];
    int fd = open("/dev/urandom", 0, 0);
    if (fd >= 0) {
        read(fd, buf, 16);
        close(fd);
    } else {
        /* Fallback: mix pid + time */
        int pid = getpid();
        struct timespec ts;
        clock_gettime(0, &ts);
        for (int i = 0; i < 16; i++) {
            buf[i] = (unsigned char)(pid * (i+1) + ts.tv_nsec * (i+3) + ts.tv_sec * (i+7));
        }
    }
    for (int i = 0; i < 16; i++) {
        printf("%02x", buf[i]);
    }
    printf("\n");
    return 0;
}
