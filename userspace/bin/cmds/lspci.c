/* lspci.c — List PCI devices by reading /sys/bus/pci/devices/ */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Read a sysfs file into static buffer */
static int read_sysfs(const char *path, char *buf, unsigned long size) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = read(fd, buf, size - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    } else {
        buf[0] = '\0';
    }
    return n;
}

int main(void) {
    /* Try reading /sys/bus/pci/devices/ first */
    int fd = open("/sys/bus/pci/devices", O_RDONLY, 0);
    if (fd < 0) {
        /* Fallback: /proc/bus/pci */
        fd = open("/proc/bus/pci/devices", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[4096];
            int n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                write(1, buf, n);
            close(fd);
            return 0;
        }
        printf("lspci: no PCI information available\n");
        return 1;
    }

    char dents[8192];
    int n = getdents64(fd, dents, sizeof(dents));
    close(fd);
    if (n <= 0) {
        printf("lspci: no PCI devices\n");
        return 0;
    }

    printf("PCI Devices:\n");
    printf("%-12s %-8s %-8s %s\n", "Slot", "Class", "Vendor", "Device");

    unsigned long pos = 0;
    while (pos < (unsigned long)n) {
        struct dirent *de = (struct dirent *)(dents + pos);
        if (de->d_name[0] != '.') {
            char path[256];
            char buf[256];
            char class_str[16] = "", vendor_str[16] = "", device_str[16] = "";

            /* Read class */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(class_str, sizeof(class_str), "0x%s", buf);
            }

            /* Read vendor */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(vendor_str, sizeof(vendor_str), "0x%s", buf);
            }

            /* Read device */
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(device_str, sizeof(device_str), "0x%s", buf);
            }

            /* Read subsystem vendor/device */
            char svendor[16] = "", sdevice[16] = "";
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_vendor", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(svendor, sizeof(svendor), " (subsys 0x%s", buf);
            }
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/subsystem_device", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(sdevice, sizeof(sdevice), ":0x%s)", buf);
            }

            /* Read IRQ */
            char irq_str[16] = "";
            snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/irq", de->d_name);
            if (read_sysfs(path, buf, sizeof(buf)) > 0) {
                snprintf(irq_str, sizeof(irq_str), " irq %s", buf);
            }

            printf("%-12s %-8s %-8s %s%s%s%s\n",
                   de->d_name, class_str, vendor_str, device_str,
                   svendor, sdevice, irq_str);
        }
        if (de->d_reclen == 0) break;
        pos += de->d_reclen;
    }

    return 0;
}
