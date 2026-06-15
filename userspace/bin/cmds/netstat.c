/* netstat.c — Show network connections */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    /* Try using net_connlist() syscall first */
    int conn_count = net_connlist();
    if (conn_count >= 0) {
        printf("Active Internet connections (servers and established)\n");
        printf("%-5s %-23s %-23s %-12s\n", "Proto", "Local Address", "Foreign Address", "State");

        if (conn_count > 0) {
            /* Try /proc/net/tcp for details */
            int fd = open("/proc/net/tcp", O_RDONLY, 0);
            if (fd >= 0) {
                char buf[4096];
                int n;
                while ((n = read(fd, buf, sizeof(buf))) > 0)
                    write(1, buf, n);
                close(fd);
            } else {
                printf("tcp   %-23s %-23s %-12s\n", "0.0.0.0:0", "0.0.0.0:0", "N/A");
                printf("  (connections: %d)\n", conn_count);
            }
        } else {
            printf("No active connections.\n");
        }
        return 0;
    }

    /* Fallback: read /proc/net/tcp directly */
    int fd = open("/proc/net/tcp", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return 0;
    }

    printf("netstat: no network information available\n");
    return 1;
}
