/* lshw.c — List hardware: read /proc/cpuinfo + /proc/meminfo */
#include "unistd.h"
#include "stdio.h"

static void cat_file(const char *path) {
    char buf[1024];
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

int main(void) {
    printf("=== CPU Info ===\n");
    cat_file("/proc/cpuinfo");
    printf("\n=== Memory Info ===\n");
    cat_file("/proc/meminfo");
    return 0;
}
