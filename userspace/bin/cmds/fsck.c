/* fsck.c — filesystem check */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Read a block from fd at given offset */
static int read_at(int fd, unsigned long long offset, void *buf, unsigned long size) {
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;
    return read(fd, buf, size);
}

const char *detect_fs_type(int fd) {
    unsigned char buf[4096];
    int n;

    /* Read primary superblock at offset 1024 (for ext2/3/4, reiserfs, etc.) */
    lseek(fd, 0, SEEK_SET);
    n = read(fd, buf, 4096);
    if (n < 0) return NULL;

    /* ext2/3/4: magic 0xEF53 at offset 1080 */
    if (n >= 1082) {
        if ((unsigned char)buf[1080] == 0x53 && (unsigned char)buf[1081] == 0xEF) {
            return "ext2/ext3/ext4";
        }
    }

    /* FAT: check boot sector at offset 0 */
    if (n >= 512) {
        /* Check for 0xAA55 signature at offset 511 */
        if (buf[511] == 0x55 && buf[510] == 0xAA) {
            /* FAT has signature bytes 0x29 or 0x28 at offset 82 in BPB */
            if (n >= 83 && (buf[82] == 0x29 || buf[82] == 0x28)) {
                return "vfat";
            }
            /* If we have 0xAA55 at end of sector, likely a FAT boot sector */
            if ((buf[0] == 0xEB && (buf[2] == 0x90 || buf[2] == 0x00)) || buf[0] == 0xE9) {
                return "vfat";
            }
        }
    }

    /* XFS: "XFSB" at offset 0 */
    if (n >= 4 && buf[0] == 'X' && buf[1] == 'F' && buf[2] == 'S' && buf[3] == 'B') {
        return "xfs";
    }

    /* ReiserFS: "ReIsErFs" at offset 0x10034 */
    {
        unsigned char reiser_buf[16];
        if (read_at(fd, 0x10034, reiser_buf, 8) == 8) {
            if (memcmp(reiser_buf, "ReIsErFs", 8) == 0 ||
                memcmp(reiser_buf, "ReIsEr2F", 8) == 0) {
                return "reiserfs";
            }
        }
    }

    /* btrfs: "_BHRfS_M" at offset 0x10040 */
    {
        unsigned char btrfs_buf[8];
        if (read_at(fd, 0x10040, btrfs_buf, 8) == 8) {
            if (memcmp(btrfs_buf, "_BHRfS_M", 8) == 0) {
                return "btrfs";
            }
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    const char *fstype = NULL;
    const char *device = NULL;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            fstype = argv[++i];
        } else if (argv[i][0] == '-') {
            printf("fsck: invalid option '%s'\n", argv[i]);
            printf("Usage: fsck [-t fstype] <device>\n");
            return 1;
        } else {
            device = argv[i];
        }
    }

    if (!device) {
        printf("Usage: fsck [-t fstype] <device>\n");
        return 1;
    }

    struct stat st;
    if (stat(device, &st) < 0) {
        printf("fsck: cannot stat '%s'\n", device);
        return 1;
    }

    printf("Device: %s\n", device);
    printf("Size: %llu bytes\n", st.st_size);

    int fd = open(device, O_RDONLY, 0);
    if (fd < 0) {
        printf("fsck: cannot open '%s'\n", device);
        return 1;
    }

    const char *detected = detect_fs_type(fd);
    if (detected) {
        printf("Filesystem: %s\n", detected);
        if (fstype && strcmp(detected, fstype) != 0) {
            printf("fsck: WARNING: expected '%s' but found '%s'\n", fstype, detected);
        }
    } else {
        printf("Filesystem: unknown\n");
        if (fstype) {
            printf("fsck: expected '%s' but could not verify\n", fstype);
        }
    }

    printf("fsck: check complete (read-only)\n");
    close(fd);
    return 0;
}
