/* lsusb.c — list USB devices using /sys/bus/usb/devices/ */
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

int main(void){
    printf("USB devices:\n");

    /* First try /sys/bus/usb/devices/ */
    int fd = open("/sys/bus/usb/devices", O_RDONLY, 0);
    if (fd >= 0) {
        char dents[8192];
        int n = getdents64(fd, dents, sizeof(dents));
        close(fd);

        if (n > 0) {
            unsigned long pos = 0;
            while (pos < (unsigned long)n) {
                struct dirent *de = (struct dirent *)(dents + pos);
                /* Show only devices with format X-Y (actual USB devices, not interfaces) */
                if (de->d_name[0] != '.' && strchr(de->d_name, '-') != NULL) {
                    char path[256];
                    char vendor[32] = "", product[32] = "", manufacturer[64] = "";
                    char product_name[128] = "", speed[16] = "";

                    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idVendor", de->d_name);
                    read_sysfs(path, vendor, sizeof(vendor));

                    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/idProduct", de->d_name);
                    read_sysfs(path, product, sizeof(product));

                    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/manufacturer", de->d_name);
                    read_sysfs(path, manufacturer, sizeof(manufacturer));

                    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/product", de->d_name);
                    read_sysfs(path, product_name, sizeof(product_name));

                    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/speed", de->d_name);
                    read_sysfs(path, speed, sizeof(speed));

                    printf("  Bus ??? Device %s: ID %s:%s", de->d_name, vendor, product);
                    if (manufacturer[0]) printf(" %s", manufacturer);
                    if (product_name[0]) printf(" %s", product_name);
                    if (speed[0]) printf(" (%s)", speed);
                    printf("\n");
                }
                if (de->d_reclen == 0) break;
                pos += de->d_reclen;
            }
        }
        return 0;
    }

    /* Fallback: try /sys/kernel/debug/usb/devices */
    fd = open("/sys/kernel/debug/usb/devices", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[8192];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            write(1, buf, n);
            return 0;
        }
    }

    /* Last fallback: show minimal info */
    printf("  No USB subsystem information available.\n");
    printf("  (Kernel must have CONFIG_USB and sysfs mounted)\n");
    return 0;
}
