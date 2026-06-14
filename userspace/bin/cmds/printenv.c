/* printenv.c — print environment variables (read /proc/self/environ) */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/self/environ", O_RDONLY, 0);
    if (fd < 0) return 1;
    char buf[4096];
    int n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = 0;
        char *p = buf;
        while (*p) {
            char *e = strchr(p, '\0');
            int len = e ? (int)(e - p) : (int)strlen(p);
            write(1, p, len);
            write(1, "\n", 1);
            if (!e) break;
            p = e + 1;
        }
    }
    close(fd);
    return 0;
}
