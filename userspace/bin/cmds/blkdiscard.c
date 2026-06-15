/* blkdiscard.c — discard sectors on device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* BLKDISCARD ioctl — Linux value: _IOR(0x12, 119, uint64_t[2]) */
#define BLKDISCARD     0x1277
#define BLKGETSIZE64   0x80081272  /* _IOR(0x12, 114, uint64_t) */

/* Simple unsigned long long parse */
static unsigned long long parse_ull(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

int main(int argc, char *argv[]) {
    unsigned long long offset = 0, length = 0;
    int have_length = 0;
    const char *device = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            offset = parse_ull(argv[++i]);
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            length = parse_ull(argv[++i]);
            have_length = 1;
        } else if (argv[i][0] == '-') {
            printf("blkdiscard: invalid option '%s'\n", argv[i]);
            printf("Usage: blkdiscard [-o offset] [-l length] <device>\n");
            return 1;
        } else {
            device = argv[i];
        }
    }

    if (!device) {
        printf("Usage: blkdiscard [-o offset] [-l length] <device>\n");
        return 1;
    }

    int fd = open(device, O_RDWR, 0);
    if (fd < 0) {
        printf("blkdiscard: cannot open '%s'\n", device);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        printf("blkdiscard: cannot stat '%s'\n", device);
        close(fd);
        return 1;
    }

    unsigned long long dev_size = 0;
    ioctl(fd, BLKGETSIZE64, &dev_size);

    if (!have_length) {
        length = dev_size - offset;
    }

    printf("Device: %s\n", device);
    printf("Size: %llu bytes\n", dev_size);
    printf("Offset: %llu\n", offset);
    printf("Length: %llu\n", length);

    unsigned long long range[2] = { offset, length };
    int ret = ioctl(fd, BLKDISCARD, range);
    if (ret < 0) {
        printf("blkdiscard: discard operation not supported or failed\n");
        close(fd);
        return 1;
    }

    printf("blkdiscard: discarded %llu bytes at offset %llu\n", length, offset);
    close(fd);
    return 0;
}
