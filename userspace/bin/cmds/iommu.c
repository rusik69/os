/* iommu.c — Show IOMMU groups */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static int read_iommu_groups(void) {
    /* Try /sys/kernel/iommu_groups/ */
    int found = 0;

    for (int grp = 0; grp < 256; grp++) {
        char dirname[64];
        snprintf(dirname, sizeof(dirname), "/sys/kernel/iommu_groups/%d", grp);

        /* Check if directory exists by opening it */
        int fd = open(dirname, O_RDONLY, 0);
        if (fd < 0)
            continue;
        close(fd);

        found = 1;
        printf("IOMMU Group %d:\n", grp);

        /* Scan devices in this group */
        char devpath[128];
        for (int dev = 0; dev < 64; dev++) {
            snprintf(devpath, sizeof(devpath),
                     "/sys/kernel/iommu_groups/%d/devices/%d", grp, dev);
            fd = open(devpath, O_RDONLY, 0);
            if (fd < 0 && dev == 0) {
                /* Try reading the directory entries via getdents */
                snprintf(devpath, sizeof(devpath),
                         "/sys/kernel/iommu_groups/%d/devices", grp);
                fd = open(devpath, O_RDONLY, 0);
                if (fd >= 0) {
                    char dents[4096];
                    int n = getdents64(fd, dents, sizeof(dents));
                    close(fd);
                    if (n > 0) {
                        unsigned long pos = 0;
                        while (pos < (unsigned long)n) {
                            struct dirent *de = (struct dirent *)&dents[pos];
                            if (de->d_name[0] != '.') {
                                printf("    %s\n", de->d_name);
                            }
                            pos += de->d_reclen;
                        }
                    }
                }
                break;
            }
            if (fd >= 0) {
                close(fd);
                printf("    Device %d\n", dev);
            } else {
                break;
            }
        }
    }

    return found;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    if (read_iommu_groups()) {
        return 0;
    }

    /* Fallback: try /proc/iommu */
    int fd = open("/proc/iommu", O_RDONLY, 0);
    if (fd >= 0) {
        char buf[4096];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(fd);
        return 0;
    }

    printf("iommu: no IOMMU information available\n");
    return 1;
}
