/* hwinfo.c — hardware info: print /proc/cpuinfo and /proc/meminfo */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

static void print_file(const char *path) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        printf("hwinfo: cannot open %s\n", path);
        return;
    }
    char buf[2048];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("=== /proc/cpuinfo ===\n");
    print_file("/proc/cpuinfo");
    printf("\n=== /proc/meminfo ===\n");
    print_file("/proc/meminfo");
    return 0;
}
