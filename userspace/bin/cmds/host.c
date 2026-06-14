/* host.c — DNS lookup: read /etc/hosts (stub) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argv;
    if (argc < 2) {
        printf("usage: host <hostname>\n");
        return 1;
    }
    int fd = open("/etc/hosts", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    printf("host: /etc/hosts not found\n");
    return 1;
}
