/* route.c — routing table (read /proc/net/route) */
#include "unistd.h"
#include "string.h"

int main(void) {
    int fd = open("/proc/net/route", O_RDONLY, 0);
    if (fd < 0) {
        const char *msg = "route: cannot open /proc/net/route\n";
        write(1, msg, strlen(msg));
        return 1;
    }
    char buf[4096];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
