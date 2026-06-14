/* pmap.c — Process memory map: read /proc/self/maps if available */
#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    int pid = 0; /* self */
    if (argc >= 2) pid = atoi(argv[1]);
    char path[64];
    if (pid == 0)
        snprintf(path, sizeof(path), "/proc/self/maps");
    else
        snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    char buf[1024];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("pmap: cannot open %s\n", path);
        return 1;
    }
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
