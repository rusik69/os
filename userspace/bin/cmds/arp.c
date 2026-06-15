/* arp.c — Show/manage ARP cache */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define IP_FMT(ip) ((ip) >> 24) & 0xFF, ((ip) >> 16) & 0xFF, ((ip) >> 8) & 0xFF, (ip) & 0xFF

int main(int argc, char *argv[]) {
    if (argc < 2) {
        /* Default: list ARP entries */
        /* Try net_arp_list() syscall first */
        int ret = net_arp_list();
        if (ret >= 0) {
            return 0;
        }

        /* Fallback: read /proc/net/arp */
        int fd = open("/proc/net/arp", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                write(1, buf, n);
                return 0;
            }
        }

        printf("arp: cannot get ARP information\n");
        return 1;
    }

    if (strcmp(argv[1], "-a") == 0) {
        /* List all ARP entries */
        int ret = net_arp_list();
        if (ret >= 0) {
            return 0;
        }

        int fd = open("/proc/net/arp", O_RDONLY, 0);
        if (fd >= 0) {
            char buf[1024];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                write(1, buf, n);
                return 0;
            }
        }

        printf("arp: no ARP entries\n");
        return 1;
    }

    if (strcmp(argv[1], "-d") == 0) {
        /* Delete ARP entry */
        if (argc < 3) {
            printf("Usage: arp -d <host>\n");
            return 1;
        }

        /* ARP deletion would use net_arp_del() syscall if available.
         * Since it's not in the header, we report it. */
        printf("arp: delete not supported by kernel syscall\n");
        return 1;
    }

    printf("Usage: arp -a              (list ARP cache)\n");
    printf("       arp -d <host>       (delete ARP entry)\n");
    return 1;
}
