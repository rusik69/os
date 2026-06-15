/* ip.c — IP networking configuration */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define IP_FMT(ip) ((unsigned int)(ip) >> 24) & 0xFF, ((unsigned int)(ip) >> 16) & 0xFF, ((unsigned int)(ip) >> 8) & 0xFF, (unsigned int)(ip) & 0xFF

static void ip_addr_show(void) {
    /* Try reading /proc/net/dev for interface list */
    int fd = open("/proc/net/dev", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return;
    }

    /* Use syscalls to show IP info */
    unsigned char ip[4], mac[6];
    int have_ip = (net_get_ip(ip) == 0);
    int have_mac = (net_get_mac(mac) == 0);
    unsigned int gw_val = net_get_gw();
    unsigned int mask_val = net_get_mask();

    printf("1: lo: <LOOPBACK> mtu 65536\n");
    printf("    link/loopback 00:00:00:00:00:00\n");
    printf("    inet 127.0.0.1/8 scope host lo\n");

    printf("2: eth0:");
    if (have_ip) {
        printf(" <BROADCAST,MULTICAST,UP>\n");
    } else {
        printf(" <NO-CARRIER,BROADCAST,MULTICAST,UP>\n");
    }
    if (have_mac) {
        printf("    link/ether %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    if (have_ip) {
        printf("    inet %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        if (mask_val) {
            printf("/%d", (int)(mask_val & 0xFF));
        }
        printf(" brd ");
        if (have_ip && mask_val) {
            unsigned int ip_val = ((unsigned int)ip[0] << 24) | ((unsigned int)ip[1] << 16) |
                                  ((unsigned int)ip[2] << 8) | ip[3];
            unsigned int brd = ip_val | ~mask_val;
            printf("%d.%d.%d.%d", IP_FMT(brd));
        } else {
            printf("0.0.0.0");
        }
        printf(" scope global eth0\n");
    }
    if (gw_val) {
        printf("    inet %d.%d.%d.%d scope global eth0\n", IP_FMT(gw_val));
    }
}

static void ip_link_set(const char *iface, const char *state) {
    (void)iface;
    (void)state;
    printf("ip: link set not supported via userspace syscall\n");
    printf("  (Use kernel shell or /sys/class/net/%s/ to set operstate)\n", iface);
}

static void ip_route_show(void) {
    /* Try reading /proc/net/route */
    int fd = open("/proc/net/route", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return;
    }

    /* Show via syscalls */
    unsigned int gw = net_get_gw();
    unsigned int mask = net_get_mask();
    unsigned char ip[4];
    int have_ip = (net_get_ip(ip) == 0);

    printf("Kernel IP routing table\n");
    printf("Destination     Gateway         Genmask         Flags Metric Ref    Use Iface\n");
    if (have_ip && mask) {
        unsigned int ip_val = ((unsigned int)ip[0] << 24) | ((unsigned int)ip[1] << 16) |
                              ((unsigned int)ip[2] << 8) | ip[3];
        unsigned int net = ip_val & mask;
        printf("%d.%d.%d.%d    ", IP_FMT(net));
        if (gw)
            printf("%d.%d.%d.%d    ", IP_FMT(gw));
        else
            printf("0.0.0.0         ");
        printf("%d.%d.%d.%d    UG    0      0       0 eth0\n", IP_FMT(mask));
    }
    printf("0.0.0.0         0.0.0.0         0.0.0.0         U     0      0       0 lo\n");
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: ip <command> [args...]\n");
        printf("commands:\n");
        printf("  ip addr show            - show addresses\n");
        printf("  ip link set <iface> up  - bring interface up\n");
        printf("  ip link set <iface> down- bring interface down\n");
        printf("  ip route show           - show routing table\n");
        return 1;
    }

    if (strcmp(argv[1], "addr") == 0) {
        if (argc >= 3 && strcmp(argv[2], "show") == 0) {
            ip_addr_show();
            return 0;
        }
        printf("ip addr: unknown subcommand\n");
        return 1;
    }

    if (strcmp(argv[1], "link") == 0) {
        if (argc >= 5 && strcmp(argv[2], "set") == 0) {
            ip_link_set(argv[3], argv[4]);
            return 0;
        }
        printf("ip link: usage: ip link set <iface> up|down\n");
        return 1;
    }

    if (strcmp(argv[1], "route") == 0) {
        if (argc >= 3 && strcmp(argv[2], "show") == 0) {
            ip_route_show();
            return 0;
        }
        printf("ip route: unknown subcommand\n");
        return 1;
    }

    printf("ip: unknown command '%s'\n", argv[1]);
    return 1;
}
