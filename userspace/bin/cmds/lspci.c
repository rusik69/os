/* lspci.c — List PCI: read /proc/bus/pci or stub */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    char buf[4096];
    int fd = open("/proc/bus/pci", O_RDONLY, 0);
    if (fd >= 0) {
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return 0;
    }
    /* Try reading /proc/bus/pci/devices */
    fd = open("/proc/bus/pci/devices", O_RDONLY, 0);
    if (fd >= 0) {
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return 0;
    }
    printf("lspci: not available\n");
    return 0;
}
