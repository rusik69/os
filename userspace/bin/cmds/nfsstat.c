/* nfsstat.c — NFS statistics: read /proc/net/rpc/nfs */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static void print_file(const char *path, const char *label) {
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) return;

    printf("%s:\n", label);
    char buf[2048];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
}

int main(void) {
    print_file("/proc/net/rpc/nfs", "NFS Client Statistics");
    print_file("/proc/net/rpc/nfsd", "NFS Server Statistics");

    /* Also try /proc/self/mountstats */
    int fd = open("/proc/self/mountstats", O_RDONLY, 0);
    if (fd >= 0) {
        printf("\nMount statistics:\n");
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            /* Filter for NFS mounts */
            char *p = buf;
            int remaining = n;
            while (remaining > 0) {
                int chunk = remaining < 4096 ? remaining : 4096;
                if (strstr(p, "nfs") || strstr(p, "NFS")) {
                    write(1, p, chunk);
                }
                p += chunk;
                remaining -= chunk;
            }
        }
        close(fd);
        return 0;
    }

    /* If nothing found */
    printf("nfsstat: no NFS statistics available\n");
    printf("  (Kernel must be built with NFS support and /proc/net/rpc/nfs)\n");
    return 1;
}
