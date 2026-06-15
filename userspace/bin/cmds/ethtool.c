/* ethtool.c — Query network device settings */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: ethtool <interface>\n");
        return 1;
    }

    const char *iface = argv[1];
    char path[128];
    char buf[256];
    int fd, n;

    printf("Settings for %s:\n", iface);

    /* Link status from /sys/class/net/<iface>/ */
    snprintf(path, sizeof(path), "/sys/class/net/%s/operstate", iface);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("    Link detected: %s\n", buf);
        }
    }

    /* Speed */
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", iface);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("    Speed: %s Mb/s\n", buf);
        }
    }

    /* Duplex */
    snprintf(path, sizeof(path), "/sys/class/net/%s/duplex", iface);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("    Duplex: %s\n", buf);
        }
    }

    /* MAC address */
    snprintf(path, sizeof(path), "/sys/class/net/%s/address", iface);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("    MAC Address: %s\n", buf);
        }
    }

    /* MTU */
    snprintf(path, sizeof(path), "/sys/class/net/%s/mtu", iface);
    fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
            printf("    MTU: %s\n", buf);
        }
    }

    /* Try to get link status via net syscalls */
    {
        unsigned char mac[6];
        if (net_get_mac(mac) == 0) {
            printf("    HW Address (syscall): %02x:%02x:%02x:%02x:%02x:%02x\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }

    /* If nothing found, report minimal info */
    if (open("/sys/class/net", O_RDONLY, 0) < 0) {
        printf("    (no sysfs network information available)\n");
    }

    return 0;
}
