/* capsh.c — capability shell: print cap info from /proc/self/status */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    int fd = open("/proc/self/status", O_RDONLY, 0);
    if (fd < 0) {
        printf("capsh: cannot open /proc/self/status\n");
        return 1;
    }
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        char *line = buf;
        while (*line) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (strncmp(line, "Cap", 3) == 0) {
                printf("%s\n", line);
            }
            if (nl) {
                *nl = '\n';
                line = nl + 1;
            } else {
                break;
            }
        }
    }
    close(fd);
    return 0;
}
