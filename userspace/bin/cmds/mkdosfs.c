/* mkdosfs.c — Create FAT32/DOS filesystem on device */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* FAT32 BPB (BIOS Parameter Block) — 512 bytes per sector */

#define SECTOR_SIZE 512
#define RESERVED_SECTORS 32
#define FAT_COUNT 2
#define CLUSTER_SIZE 1  /* sectors per cluster */

/* FAT32 FSInfo sector signature */
#define FSINFO_SIG1 0x41615252
#define FSINFO_SIG2 0x61417272
#define FSINFO_SIG3 0xAA550000

static void write_le16(unsigned char *p, unsigned short v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void write_le32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: mkdosfs DEVICE [BLOCK_SIZE]\n");
        return 1;
    }

    const char *device = argv[1];

    int fd = open(device, O_RDWR, 0);
    if (fd < 0) {
        printf("mkdosfs: cannot open '%s'\n", device);
        return 1;
    }

    /* Determine device size */
    unsigned long long total_sectors = 0;
    /* Use lseek to end to find size */
    long long end = lseek(fd, 0, SEEK_END);
    if (end > 0) {
        total_sectors = (unsigned long long)end / SECTOR_SIZE;
    }
    lseek(fd, 0, SEEK_SET);

    if (total_sectors < 65536) {
        printf("mkdosfs: device too small for FAT32 (%llu sectors)\n", total_sectors);
        close(fd);
        return 1;
    }

    /* Calculate FAT geometry */
    unsigned int total_clusters = (unsigned int)((total_sectors - RESERVED_SECTORS) /
                                   (FAT_COUNT + CLUSTER_SIZE));
    unsigned int fat_sectors = (total_clusters * 4 + SECTOR_SIZE - 1) / SECTOR_SIZE;
    unsigned int data_sectors = (unsigned int)total_sectors - RESERVED_SECTORS - fat_sectors * FAT_COUNT;
    unsigned int root_cluster = 2;

    /* Allocate a sector buffer */
    unsigned char sector[SECTOR_SIZE];
    memset(sector, 0, sizeof(sector));

    /* ── Sector 0: Boot sector / BPB ────────────────────────────── */
    /* Jump code */
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;
    memcpy(sector + 3, "MSDOS5.0", 8);

    /* BPB */
    write_le16(sector + 11, SECTOR_SIZE);       /* bytes per sector */
    sector[13] = CLUSTER_SIZE;                   /* sectors per cluster */
    write_le16(sector + 14, RESERVED_SECTORS);   /* reserved sectors */
    sector[16] = FAT_COUNT;                      /* number of FATs */
    write_le16(sector + 17, 0);                  /* root entries (0 for FAT32) */
    write_le16(sector + 19, 0);                  /* total sectors 16-bit (0 for FAT32) */
    sector[21] = 0xF8;                           /* media descriptor (fixed disk) */
    write_le16(sector + 22, 0);                  /* FAT size 16-bit (0 for FAT32) */
    write_le16(sector + 24, 63);                 /* sectors per track */
    write_le16(sector + 26, 255);                /* heads */
    write_le32(sector + 28, 0);                  /* hidden sectors */
    write_le32(sector + 32, (unsigned int)(total_sectors & 0xFFFFFFFF));  /* total sectors 32-bit */

    /* FAT32 extended fields */
    write_le32(sector + 36, fat_sectors);        /* sectors per FAT */
    write_le16(sector + 40, 0);                  /* extended flags */
    write_le16(sector + 42, 0);                  /* FS version */
    write_le32(sector + 44, root_cluster);       /* root dir cluster */
    write_le16(sector + 48, 1);                  /* FSInfo sector */
    write_le16(sector + 50, 6);                  /* backup boot sector */
    memset(sector + 52, 0, 12);                  /* reserved */
    sector[64] = 0x80;                           /* drive number (hard disk) */
    sector[65] = 0;                              /* reserved (NT flags) */
    sector[66] = 0x29;                           /* extended boot signature */
    write_le32(sector + 67, 0x12345678);         /* volume ID serial number */
    memcpy(sector + 71, "NO NAME    ", 11);      /* volume label */
    memcpy(sector + 82, "FAT32   ", 8);          /* filesystem type */

    /* Signature */
    write_le16(sector + 510, 0xAA55);

    /* Write boot sector */
    write(fd, sector, SECTOR_SIZE);

    /* ── Sector 1: FSInfo sector ────────────────────────────────── */
    memset(sector, 0, sizeof(sector));
    write_le32(sector + 0, FSINFO_SIG1);
    memset(sector + 4, 0, 480);
    write_le32(sector + 484, FSINFO_SIG2);
    write_le32(sector + 488, root_cluster + 1);  /* free cluster count hint */
    write_le32(sector + 492, root_cluster + 1);  /* next free cluster hint */
    memset(sector + 496, 0, 12);
    write_le32(sector + 508, FSINFO_SIG3);
    write(fd, sector, SECTOR_SIZE);

    /* ── Sector 2-5: Backup boot sector + backup FSInfo ────────── */
    /* Skip to backup boot sector (sector 6) */
    lseek(fd, (long)(6 * SECTOR_SIZE), SEEK_SET);
    /* Write backup boot sector (same as sector 0) */
    unsigned char bootsec[SECTOR_SIZE];
    memcpy(bootsec, sector, sizeof(bootsec));
    bootsec[510] = 0x55;
    bootsec[511] = 0xAA;
    write(fd, bootsec, SECTOR_SIZE);

    /* ── Reserved sectors (fill with zeros) ─────────────────────── */
    lseek(fd, (long)(RESERVED_SECTORS * SECTOR_SIZE), SEEK_SET);

    /* ── FAT: cluster 0 and 1 reserved, cluster 2 = EOC for root ─ */
    memset(sector, 0, sizeof(sector));
    /* Cluster 0: media descriptor (0x0FFFFFF8) */
    write_le32(sector + 0, 0x0FFFFFF8);
    /* Cluster 1: EOC (end of cluster chain) */
    write_le32(sector + 4, 0x0FFFFFFF);
    /* Cluster 2: EOC (root directory) */
    write_le32(sector + 8, 0x0FFFFFFF);

    for (unsigned int fat = 0; fat < FAT_COUNT; fat++) {
        lseek(fd, (long)((RESERVED_SECTORS + fat * fat_sectors) * SECTOR_SIZE), SEEK_SET);
        write(fd, sector, SECTOR_SIZE);
        /* Write remaining FAT sectors as zero */
        unsigned char zeros[SECTOR_SIZE];
        memset(zeros, 0, sizeof(zeros));
        for (unsigned int s = 1; s < fat_sectors; s++) {
            write(fd, zeros, SECTOR_SIZE);
        }
    }

    /* ── Root directory cluster ─────────────────────────────────── */
    memset(sector, 0, sizeof(sector));
    lseek(fd, (long)((RESERVED_SECTORS + fat_sectors * FAT_COUNT) * SECTOR_SIZE), SEEK_SET);
    /* Write one cluster of zeros for root directory */
    for (unsigned int s = 0; s < CLUSTER_SIZE; s++) {
        write(fd, sector, SECTOR_SIZE);
    }

    close(fd);

    printf("mkdosfs: FAT32 filesystem created on %s\n", device);
    printf("  Sectors: %llu, FAT sectors: %u, Clusters: %u\n",
           total_sectors, fat_sectors, data_sectors);
    return 0;
}
