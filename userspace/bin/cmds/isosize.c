/* isosize.c — display ISO filesystem size */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

#define ISO_SECTOR_SIZE 2048
#define PVD_SECTOR 16
#define PVD_OFFSET 80   /* Volume Space Size (LE32) */
#define PVD_OFFSET_BE 84 /* Volume Space Size (BE32) */

int main(int argc, char *argv[]) {
    int show_sectors = 0;
    const char *file;
    int argidx = 1;

    if (argc < 2) {
        printf("usage: isosize [-x] <iso-file>\n");
        printf("  -x    show sector count instead of byte count\n");
        return 1;
    }

    if (strcmp(argv[1], "-x") == 0) {
        show_sectors = 1;
        argidx = 2;
    }

    if (argidx >= argc) {
        printf("usage: isosize [-x] <iso-file>\n");
        return 1;
    }
    file = argv[argidx];

    int fd = open(file, O_RDONLY, 0);
    if (fd < 0) {
        printf("isosize: cannot open '%s'\n", file);
        return 1;
    }

    /* Seek to Primary Volume Descriptor (sector 16) */
    long pvd_offset = (long)PVD_SECTOR * ISO_SECTOR_SIZE;
    if (lseek(fd, pvd_offset, SEEK_SET) < 0) {
        printf("isosize: seek failed\n");
        close(fd);
        return 1;
    }

    /* Read PVD (one sector) */
    unsigned char pvd[ISO_SECTOR_SIZE];
    int n = read(fd, pvd, ISO_SECTOR_SIZE);
    close(fd);
    if (n < ISO_SECTOR_SIZE) {
        printf("isosize: cannot read PVD\n");
        return 1;
    }

    /* Validate PVD: first byte should be 1 (PVD), second byte should be 'CD001' */
    if (pvd[0] != 1 || pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1') {
        printf("isosize: not a valid ISO 9660 image\n");
        return 1;
    }

    /* Volume Space Size at offset 80 (LE32) and 84 (BE32) */
    unsigned int vol_size_le = (unsigned int)pvd[PVD_OFFSET] |
                               ((unsigned int)pvd[PVD_OFFSET + 1] << 8) |
                               ((unsigned int)pvd[PVD_OFFSET + 2] << 16) |
                               ((unsigned int)pvd[PVD_OFFSET + 3] << 24);

    unsigned int vol_size_be = (unsigned int)pvd[PVD_OFFSET_BE] << 24 |
                               ((unsigned int)pvd[PVD_OFFSET_BE + 1] << 16) |
                               ((unsigned int)pvd[PVD_OFFSET_BE + 2] << 8) |
                               (unsigned int)pvd[PVD_OFFSET_BE + 3];

    /* LE is preferred; BE is validation */
    unsigned int vol_size = vol_size_le;
    if (vol_size_le != vol_size_be) {
        /* Use LE; cross-check failed silently */
    }

    if (show_sectors) {
        printf("%u\n", vol_size);
    } else {
        unsigned long long bytes = (unsigned long long)vol_size * ISO_SECTOR_SIZE;
        printf("%llu\n", bytes);
    }

    return 0;
}
