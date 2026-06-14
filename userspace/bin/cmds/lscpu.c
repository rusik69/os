/* lscpu.c — show CPU info */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to read /proc/cpuinfo */
    int fd = open("/proc/cpuinfo", 0, 0);
    if (fd >= 0) {
        char buf[4096];
        long n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
        return 0;
    }
    /* Fallback: print from uname */
    struct utsname uts;
    if (uname(&uts) == 0) {
        printf("Architecture:    %s\n", uts.machine);
        printf("CPU op-mode(s):  64-bit\n");
        printf("Byte Order:      Little Endian\n");
        printf("CPU(s):          1\n");
        printf("Vendor ID:       GenuineIntel\n");
        printf("Model name:      QEMU Virtual CPU\n");
    } else {
        printf("lscpu: unable to determine CPU info\n");
    }
    return 0;
}
