/* nstat.c — network stats (read /proc/net/netstat) */
#include "unistd.h"
#include "string.h"

int main(void) {
    int fd = open("/proc/net/netstat", O_RDONLY, 0);
    if (fd < 0) {
        const char *msg = "nstat: cannot open /proc/net/netstat\n";
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
