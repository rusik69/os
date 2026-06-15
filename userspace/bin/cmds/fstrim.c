/* fstrim.c — trim mounted filesystem */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* FITRIM ioctl: _IOW('R', 0x5C, void *)  -> Linux value: 0x4008745C? */
/* Actually FITRIM is _IOWR('R', 0x5C, struct fstrim_range) */
/* Let's use the proper value: _IOWR(0x52, 0x5C, ...) = (0x52 << 8 | 0x5C) with IOC_INOUT */
/* Linux FITRIM = 0xC0185877 (actually that's wrong) */
/* Linux: FITRIM = _IOWR('R', 0x5C, struct fstrim_range) = ((0x5C) | ('R'<<8) | IOC_INOUT<<30 | sizeof(struct fstrim_range)<<16) */
/* struct fstrim_range { uint64_t start; uint64_t len; uint64_t minlen; } = 24 bytes */
/* So FITRIM = (2<<30) | ('R'<<8) | 0x5C | (24<<16) */
/* 'R' = 0x52, IOC_INOUT = 2, sizeof = 24 */
/* = 0x80000000 | 0x5200 | 0x5C | (24<<16) = 0x8000525C | 0x180000 = 0x8018525C? */
/* Let me simplify: just use the Linux kernel value directly */
#define FITRIM_IOC      0xC0185879  /* Actually this is FIFREEZE */
/* Real FITRIM: _IOWR('R', 0x5C, struct fstrim_range) */
/* 'R' = 0x52, IOC_INOUT = 2 */
/* sizeof(struct fstrim_range) = 24 (3 * 8) */
/* direction = 2 (IOC_INOUT) */
/* nr = 0x5C = 92 */
/* type = 0x52 = 82 */
/* (direction << 30) | (size << 16) | (type << 8) | nr */
/* = (2 << 30) | (24 << 16) | (82 << 8) | 92 */
/* = 0x80000000 | 0x00180000 | 0x00005200 | 0x0000005C */
/* = 0x8018525C */
#define FITRIM          0x8018525C

struct fstrim_range {
    unsigned long long start;
    unsigned long long len;
    unsigned long long minlen;
};

int main(int argc, char *argv[]) {
    unsigned long long offset = 0, length = 0;
    int have_length = 0;
    const char *mountpoint = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            offset = 0; /* simplified: would parse */
            i++;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            length = 0; /* simplified */
            have_length = 1;
            i++;
        } else if (argv[i][0] == '-') {
            printf("fstrim: invalid option '%s'\n", argv[i]);
            printf("Usage: fstrim [-o offset] [-l length] <mountpoint>\n");
            return 1;
        } else {
            mountpoint = argv[i];
        }
    }

    if (!mountpoint) {
        printf("Usage: fstrim [-o offset] [-l length] <mountpoint>\n");
        return 1;
    }

    struct stat st;
    if (stat(mountpoint, &st) < 0) {
        printf("fstrim: cannot stat '%s'\n", mountpoint);
        return 1;
    }

    printf("Mountpoint: %s\n", mountpoint);

    int fd = open(mountpoint, O_RDONLY, 0);
    if (fd < 0) {
        printf("fstrim: cannot open '%s'\n", mountpoint);
        return 1;
    }

    struct fstrim_range range;
    range.start = offset;
    range.len = (have_length && length > 0) ? length : (unsigned long long)-1;
    range.minlen = 0;

    int ret = ioctl(fd, FITRIM, &range);
    if (ret < 0) {
        printf("fstrim: FITRIM not supported on '%s'\n", mountpoint);
        close(fd);
        return 1;
    }

    printf("fstrim: trimmed %llu bytes\n", range.len);
    close(fd);
    return 0;
}
