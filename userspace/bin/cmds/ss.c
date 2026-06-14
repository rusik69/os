/* ss.c — socket statistics (read /proc/net/tcp) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/net/tcp", O_RDONLY, 0);
    if (fd < 0) {
        const char *msg = "ss: cannot open /proc/net/tcp\n";
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
