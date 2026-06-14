/* tty.c — print terminal device name */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try /proc/self/status for tty info */
    int fd = open("/proc/self/status", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[512];
        int n = read(fd, buf, 511);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            char *p = strstr(buf, "Tty:");
            if (p) {
                p += 4;
                while (*p == ' ' || *p == '\t') p++;
                char *e = p;
                while (*e && *e != '\n' && *e != '\r') e++;
                char saved = *e;
                *e = '\0';
                printf("%s\n", p);
                *e = saved;
                return 0;
            }
        }
    }
    printf("/dev/console\n");
    return 0;
}
