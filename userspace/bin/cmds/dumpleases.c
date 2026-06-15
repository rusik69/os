/* dumpleases.c — DHCP leases: read /var/lib/dhcp/ or fallback to net_arp_list */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Try reading DHCP lease files */
    static const char *lease_paths[] = {
        "/var/lib/dhcp/dhcpd.leases",
        "/var/lib/dhcp/leases",
        "/var/lib/dhcp/dhclient.leases",
        NULL
    };

    for (int i = 0; lease_paths[i] != NULL; i++) {
        int fd = open(lease_paths[i], O_RDONLY, 0);
        if (fd >= 0) {
            char buf[4096];
            int n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) {
                buf[n] = '\0';
                printf("DHCP leases from %s:\n", lease_paths[i]);
                printf("%s", buf);
                return 0;
            }
        }
    }

    /* Fallback: use ARP list */
    printf("DHCP lease file not found.\n");
    printf("Active ARP entries:\n");
    int ret = net_arp_list();
    if (ret < 0) {
        printf("dumpleases: no lease information available\n");
        return 1;
    }

    return 0;
}
