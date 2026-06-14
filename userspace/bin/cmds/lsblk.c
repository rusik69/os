/* lsblk.c — list block devices */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    /* Try to read /sys/block directory */
    int fd = open("/sys/block", O_RDONLY, 0);
    if (fd < 0) {
        printf("lsblk: cannot open /sys/block (no sysfs mounted)\n");
        printf("NAME  MAJ:MIN  SIZE TYPE MOUNTPOINT\n");
        printf("(no block devices available)\n");
        return 1;
    }
    char buf[2048];
    int n = getdents64(fd, buf, 2048);
    close(fd);
    if (n <= 0) {
        printf("lsblk: no block devices found\n");
        return 0;
    }
    printf("NAME  MAJ:MIN  SIZE TYPE\n");
    int pos = 0;
    while (pos < n) {
        struct dirent *d = (struct dirent *)(buf + pos);
        if (d->d_name[0] != '.') {
            printf("%-4s  ", d->d_name);
            printf("0:0    ");
            printf("0   ");
            printf("disk\n");
        }
        pos += d->d_reclen;
    }
    return 0;
}
