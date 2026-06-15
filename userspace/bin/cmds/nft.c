/* nft.c — nftables configuration: read /proc/net/nft or display kernel info */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void show_nftables(void) {
    /* Try /proc/net/nft first (kernel nftables proc interface) */
    int fd = open("/proc/net/nft", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return;
    }

    /* Try /proc/net/nf_tables or /proc/net/netfilter */
    fd = open("/proc/net/nf_tables", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return;
    }

    /* Fallback: show kernel nftables system information */
    printf("nftables configuration:\n");

    /* Check if nf_tables module is loaded */
    fd = open("/proc/modules", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "nf_tables")) {
                printf("  Status: nf_tables module loaded\n");
            } else {
                printf("  Status: nf_tables module not loaded\n");
            }
        }
    }

    /* Show available tables from /proc/net/netfilter */
    fd = open("/proc/net/netfilter", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            write(1, buf, n);
        }
        close(fd);
    } else {
        printf("  Tables: (kernel provides nftables via netlink, /proc not available)\n");
        printf("  Use 'nft list ruleset' from kernel shell for full rules.\n");
    }
}

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "list") == 0) {
            if (argc > 2 && strcmp(argv[2], "ruleset") == 0) {
                show_nftables();
                return 0;
            }
            show_nftables();
            return 0;
        }
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            printf("Usage: nft list ruleset  (list nftables rules)\n");
            printf("       nft add/delete     (modification not available)\n");
            return 0;
        }
        printf("nft: unknown option '%s'\n", argv[1]);
        printf("Usage: nft list ruleset\n");
        return 1;
    }

    show_nftables();
    return 0;
}
