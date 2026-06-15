/* conntrack.c — Show connection tracking entries */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    int list_mode = 0;

    if (argc > 1) {
        if (strcmp(argv[1], "-L") == 0) {
            list_mode = 1;
        } else {
            printf("Usage: conntrack -L  (list connection tracking entries)\n");
            return 1;
        }
    } else {
        list_mode = 1;
    }

    if (list_mode) {
        /* Try to read /proc/net/nf_conntrack or /proc/net/ip_conntrack */
        static const char *paths[] = {
            "/proc/net/nf_conntrack",
            "/proc/net/ip_conntrack",
            NULL
        };

        for (int i = 0; paths[i] != NULL; i++) {
            int fd = open(paths[i], O_RDONLY, 0);
            if (fd >= 0) {
                char buf[4096];
                int n;
                while ((n = read(fd, buf, sizeof(buf))) > 0)
                    write(1, buf, n);
                close(fd);
                return 0;
            }
        }

        printf("conntrack: connection tracking table not available\n");
        printf("  (Kernel must be built with CONFIG_NETFILTER and CT support)\n");
        return 1;
    }

    return 0;
}
