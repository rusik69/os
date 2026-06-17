/* blkid.c — locate/print block device attributes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

/* Simple unsigned long long parse from string */
static unsigned long long parse_ull(const char *s) {
    unsigned long long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

/* Detect filesystem type by superblock magic */
static const char *detect_fs(int fd) {
    unsigned char buf[4096];
    int n;

    /* Read superblock (offset 1024 for most fs) */
    n = read(fd, buf, 4096);
    if (n < 0) return NULL;

    /* ext2/3/4: magic 0xEF53 at offset 1080 from start (or offset 56 in superblock) */
    if (n >= 1080 + 2 && buf[1080] == 0x53 && buf[1081] == 0xEF) {
        /* Check for ext features to distinguish */
        return "ext2/ext3/ext4";
    }

    /* btrfs: "_BHRfS_M" at offset 0x10040 (or at offset 64 in superblock at 0x10000) */
    /* Actually btrfs superblock is at 0x10000, magic at offset 0x10040 */
    {
        char btrfs_magic[8] = {'_','B','H','R','f','S','_','M'};
        char tmp[8];
        if (lseek(fd, 0x10040, SEEK_SET) >= 0 && read(fd, tmp, 8) == 8) {
            if (memcmp(tmp, btrfs_magic, 8) == 0)
                return "btrfs";
        }
        /* Also try at offset 64 of superblock at 0x10000 => 0x10040 already */
    }

    /* ReiserFS: "ReIsErFs" or "ReIsEr2Fs" at offset 0x10000 + 0x34 */
    {
        char reiser_magic[8] = {'R','e','I','s','E','r','F','s'};
        char tmp[8];
        if (lseek(fd, 0x10034, SEEK_SET) >= 0 && read(fd, tmp, 8) == 8) {
            if (memcmp(tmp, reiser_magic, 8) == 0)
                return "reiserfs";
        }
        /* Also try "ReIsEr2Fs" */
        char reiser2_magic[10] = "ReIsEr2Fs";
        char tmp2[10];
        if (lseek(fd, 0x10034, SEEK_SET) >= 0 && read(fd, tmp2, 10) == 10) {
            if (memcmp(tmp2, reiser2_magic, 10) == 0)
                return "reiserfs";
        }
    }

    /* XFS: "XFSB" at offset 0 */
    if (n >= 4 && buf[0] == 'X' && buf[1] == 'F' && buf[2] == 'S' && buf[3] == 'B')
        return "xfs";

    /* FAT: 0x29 or 0x28 at offset 510/511 (boot sector signature 0xAA55 at 511/512) */
    if (n >= 512 && buf[510] == 0x55 && buf[511] == 0xAA) {
        if (buf[0] == 0xEB || buf[0] == 0xE9) {
            /* Check for FAT32 vs FAT16 by BPB */
            /* Check for 0x29 or 0x28 at offset 82 in BPB */
            if (n >= 83 && (buf[82] == 0x29 || buf[82] == 0x28))
                return "vfat";
            return "vfat";
        }
    }

    /* Check offset 0 for FAT32 extended boot record */
    if (n >= 512 && buf[0] == 0xEB && buf[2] == 0x90) {
        if (n >= 83 && (buf[82] == 0x29 || buf[82] == 0x28))
            return "vfat";
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    const char *device = NULL;

    if (argc > 1 && argv[1][0] != '-') {
        device = argv[1];
    }

    /* Scan /sys/block/ for block devices */
    /* List block devices and print info */
    int found = 0;

    if (device) {
        /* Print info for specific device */
        struct stat st;
        if (stat(device, &st) < 0) {
            printf("blkid: cannot stat '%s'\n", device);
            return 1;
        }
        printf("DEVICE: %s\n", device);
        printf("SIZE: %llu\n", st.st_size);

        int fd = open(device, O_RDONLY, 0);
        if (fd >= 0) {
            const char *fs = detect_fs(fd);
            if (fs) {
                printf("TYPE: %s\n", fs);
            } else {
                printf("TYPE: unknown\n");
            }
            close(fd);
        }
        found = 1;
    } else {
        /* List all block devices from /sys/block */
        int sys_fd = open("/sys/block", O_RDONLY, 0);
        if (sys_fd < 0) {
            printf("blkid: cannot access /sys/block\n");
            return 1;
        }
        char buf[4096];
        int n = getdents64(sys_fd, buf, sizeof(buf));
        close(sys_fd);
        if (n < 0) {
            printf("blkid: cannot read /sys/block\n");
            return 1;
        }
        int pos = 0;
        while (pos < n) {
            struct dirent *de = (struct dirent *)(buf + pos);
            if (de->d_name[0] != '.') {
                char path[256];
                snprintf(path, sizeof(path), "/dev/%s", de->d_name);
                printf("DEVICE: %s\n", path);

                /* Try to get size */
                char size_path[256];
                snprintf(size_path, sizeof(size_path), "/sys/block/%s/size", de->d_name);
                int fd = open(size_path, O_RDONLY, 0);
                if (fd >= 0) {
                    char s[64];
                    int m = read(fd, s, sizeof(s) - 1);
                    if (m > 0) {
                        s[m] = 0;
                        unsigned long long sectors = parse_ull(s);
                        printf("SIZE: %llu bytes (%llu sectors)\n", sectors * 512, sectors);
                    }
                    close(fd);
                }

                /* Try to detect filesystem */
                char dev_path[256];
                snprintf(dev_path, sizeof(dev_path), "/dev/%s", de->d_name);
                int dev_fd = open(dev_path, O_RDONLY, 0);
                if (dev_fd >= 0) {
                    const char *fs = detect_fs(dev_fd);
                    if (fs) {
                        printf("TYPE: %s\n", fs);
                    }
                    close(dev_fd);
                }

                printf("\n");
                found = 1;
            }
            if (de->d_reclen == 0) break;
            pos += de->d_reclen;
        }
    }

    if (!found) {
        printf("blkid: no devices found\n");
        return 1;
    }
    return 0;
}
