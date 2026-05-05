/*
 * FAT32 Read-Only Driver
 *
 * Supports reading files and listing directories from a FAT32 partition.
 * Works with either the legacy PIO-ATA driver or the new AHCI driver.
 *
 * Partition table auto-detection: reads MBR, finds first type 0x0B/0x0C
 * (FAT32) or 0xOE (FAT32 LBA) partition.
 *
 * Path convention: UNIX-style with '/' separator, case-insensitive against
 * the 8.3 and long-name directory entries.
 */
#include "fat32.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"

/* ── FAT32 on-disk structures ───────────────────────────────────────────────── */
struct fat32_bpb {
    uint8_t  jump[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    /* 0 for FAT32 */
    uint16_t total_sectors_16;    /* 0 for FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        /* first cluster of root directory */
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];          /* "FAT32   " */
} __attribute__((packed));

struct fat32_dirent {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t file_size;
} __attribute__((packed));

/* LFN (Long File Name) entry */
struct fat32_lfn {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;           /* must be 0x0F */
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster;        /* always 0 */
    uint16_t name3[2];
} __attribute__((packed));

#define FAT32_ATTR_READ_ONLY  0x01
#define FAT32_ATTR_HIDDEN     0x02
#define FAT32_ATTR_SYSTEM     0x04
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20
#define FAT32_ATTR_LFN        0x0F   /* all of RO|HIDDEN|SYSTEM|VOLUME_ID */

#define FAT32_EOC    0x0FFFFFF8u     /* end-of-chain marker */
#define FAT32_FREE   0x00000000u

/* ── Driver state ───────────────────────────────────────────────────────────── */
static int          mounted       = 0;
static fat32_disk_t disk_type     = FAT32_DISK_ATA;
static int          disk_id       = BLOCKDEV_ATA;
static uint32_t     part_start    = 0;   /* LBA of partition start */
static uint32_t     fat_start     = 0;   /* LBA of FAT region */
static uint32_t     data_start    = 0;   /* LBA of cluster 2 */
static uint32_t     root_cluster  = 2;
static uint32_t     spc           = 0;   /* sectors per cluster */
static uint32_t     bps           = 512; /* bytes per sector */

/* Sector-sized scratch buffer (on stack in helpers) */
#define SECT_SIZE 512

/* ── Low-level sector read ──────────────────────────────────────────────────── */
static int read_sector(uint32_t lba, void *buf) {
    return blockdev_read_sectors(disk_id, lba, 1, buf);
}

/* ── FAT chain traversal ────────────────────────────────────────────────────── */
static uint32_t fat_next_cluster(uint32_t cluster) {
    /* Each FAT entry = 4 bytes; 128 entries per 512-byte sector */
    uint32_t fat_sector = fat_start + (cluster * 4) / SECT_SIZE;
    uint32_t offset     = (cluster * 4) % SECT_SIZE;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fat_sector, buf) != 0) return 0x0FFFFFFF;
    uint32_t val;
    __builtin_memcpy(&val, buf + offset, 4);
    return val & 0x0FFFFFFF;
}

/* Cluster number → LBA of first sector */
static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start + (cluster - 2) * spc;
}

/* ── 8.3 name comparison (case-insensitive) ─────────────────────────────────── */
static int name83_match(const char name8[8], const char ext3[3], const char *want) {
    /* Build "NAME.EXT" or "NAME" */
    char buf[13];
    int  ni = 0;
    for (int i = 0; i < 8 && name8[i] != ' '; i++) {
        char c = name8[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        buf[ni++] = c;
    }
    if (ext3[0] != ' ') {
        buf[ni++] = '.';
        for (int i = 0; i < 3 && ext3[i] != ' '; i++) {
            char c = ext3[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            buf[ni++] = c;
        }
    }
    buf[ni] = '\0';

    /* Compare case-insensitively */
    int j = 0;
    for (; buf[j] && want[j]; j++) {
        char a = buf[j], b = want[j];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return 0;
    }
    return buf[j] == '\0' && want[j] == '\0';
}

/* ── Traverse a directory cluster chain looking for a name component ─────────
 * Returns first cluster of the found entry, or 0.
 * Sets *is_dir if the entry is a directory.
 * Sets *file_size to the file size.
 */
static uint32_t dir_find(uint32_t dir_cluster, const char *name,
                          int *is_dir, uint32_t *file_size) {
    uint8_t buf[SECT_SIZE];
    uint32_t cluster = dir_cluster;

    while (cluster < FAT32_EOC) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0) return 0;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return 0;  /* end of directory */
                if (first == 0xE5) continue;  /* deleted entry */
                if (entries[i].attr == FAT32_ATTR_LFN) continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;
                if (name83_match(entries[i].name, entries[i].ext, name)) {
                    uint32_t clus = ((uint32_t)entries[i].cluster_hi << 16)
                                  | entries[i].cluster_lo;
                    if (is_dir)    *is_dir    = !!(entries[i].attr & FAT32_ATTR_DIRECTORY);
                    if (file_size) *file_size = entries[i].file_size;
                    return clus ? clus : root_cluster; /* root dir cluster may be 0 */
                }
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

/* ── Resolve a path → starting cluster of the last component ────────────────── */
static uint32_t path_resolve(const char *path, int *is_dir, uint32_t *file_size) {
    /* Skip leading '/' */
    while (*path == '/') path++;

    uint32_t cluster = root_cluster;
    if (*path == '\0') {
        if (is_dir)    *is_dir    = 1;
        if (file_size) *file_size = 0;
        return cluster;
    }

    while (*path) {
        /* Extract next component */
        char comp[FAT32_MAX_NAME];
        int  ci = 0;
        while (*path && *path != '/') comp[ci++] = *path++;
        comp[ci] = '\0';
        while (*path == '/') path++;

        int     d = 0;
        uint32_t fs = 0;
        uint32_t next = dir_find(cluster, comp, &d, &fs);
        if (!next) return 0;

        cluster = next;
        if (is_dir)    *is_dir    = d;
        if (file_size) *file_size = fs;

        if (*path && !d) return 0;  /* not a directory but more path remains */
    }
    return cluster;
}

/* ── Public API ─────────────────────────────────────────────────────────────── */
int fat32_is_mounted(void) { return mounted; }

int fat32_mount(fat32_disk_t disk, uint32_t part_lba) {
    disk_type = disk;
    mounted   = 0;

    switch (disk_type) {
        case FAT32_DISK_AHCI:
            disk_id = BLOCKDEV_AHCI;
            break;
        case FAT32_DISK_USB0:
            disk_id = BLOCKDEV_USB0;
            break;
        case FAT32_DISK_ATA:
        default:
            disk_id = BLOCKDEV_ATA;
            break;
    }
    if (!blockdev_is_registered(disk_id)) return -10;

    uint8_t mbr[SECT_SIZE];
    /* Auto-detect: if part_lba == 0, read MBR and find FAT32 partition */
    if (part_lba == 0) {
        if (read_sector(0, mbr) != 0) return -1;
        /* MBR signature */
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -2;
        /* Partition table entries at offset 0x1BE (4 entries × 16 bytes) */
        uint8_t *pt = mbr + 0x1BE;
        for (int i = 0; i < 4; i++) {
            uint8_t ptype = pt[i * 16 + 4];
            if (ptype == 0x0B || ptype == 0x0C || ptype == 0x0E) {
                uint32_t start;
                __builtin_memcpy(&start, pt + i * 16 + 8, 4);
                part_lba = start;
                break;
            }
        }
        if (part_lba == 0) return -3;  /* no FAT32 partition found */
    }

    /* Read BPB */
    uint8_t boot[SECT_SIZE];
    if (read_sector(part_lba, boot) != 0) return -4;

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot;

    /* Validate FAT32 */
    if (__builtin_memcmp(bpb->fs_type, "FAT32   ", 8) != 0 &&
        __builtin_memcmp(bpb->fs_type, "FAT32", 5) != 0) return -5;

    bps          = bpb->bytes_per_sector;
    spc          = bpb->sectors_per_cluster;
    root_cluster = bpb->root_cluster;
    fat_start    = part_lba + bpb->reserved_sectors;
    data_start   = fat_start + (uint32_t)bpb->num_fats * bpb->fat_size_32;
    part_start   = part_lba;
    mounted      = 1;

    kprintf("  FAT32 mounted: vol='%.11s' root_clus=%u\n",
            bpb->volume_label, (uint64_t)root_cluster);
    return 0;
}

int fat32_file_size(const char *path) {
    if (!mounted) return -1;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t clus = path_resolve(path, &is_dir, &fsize);
    if (!clus || is_dir) return -1;
    return (int)fsize;
}

int fat32_read_file(const char *path, void *buf, uint32_t max_size) {
    if (!mounted) return -1;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t cluster = path_resolve(path, &is_dir, &fsize);
    if (!cluster || is_dir) return -1;

    uint32_t to_read = fsize < max_size ? fsize : max_size;
    uint32_t done    = 0;
    uint8_t  sect_buf[SECT_SIZE];
    uint32_t clus = cluster;

    while (clus < FAT32_EOC && done < to_read) {
        uint32_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && done < to_read; s++) {
            if (read_sector(lba + s, sect_buf) != 0) return (int)done;
            uint32_t chunk = SECT_SIZE;
            if (chunk > to_read - done) chunk = to_read - done;
            __builtin_memcpy((uint8_t *)buf + done, sect_buf, chunk);
            done += chunk;
        }
        clus = fat_next_cluster(clus);
    }
    return (int)done;
}

int fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max) {
    if (!mounted) return -1;
    int is_dir = 0;
    uint32_t cluster = path_resolve(path, &is_dir, (void *)0);
    if (!cluster || !is_dir) return -1;

    int count = 0;
    uint8_t buf[SECT_SIZE];
    uint32_t clus = cluster;

    while (clus < FAT32_EOC && count < max) {
        uint32_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && count < max; s++) {
            if (read_sector(lba + s, buf) != 0) return count;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) goto done;
                if (first == 0xE5) continue;
                if (entries[i].attr == FAT32_ATTR_LFN) continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) continue;
                /* Build "name.ext" */
                char *out = names[count];
                int ni = 0;
                for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                    char c = entries[i].name[j];
                    if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                    out[ni++] = c;
                }
                if (entries[i].ext[0] != ' ') {
                    out[ni++] = '.';
                    for (int j = 0; j < 3 && entries[i].ext[j] != ' '; j++) {
                        char c = entries[i].ext[j];
                        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
                        out[ni++] = c;
                    }
                }
                if (entries[i].attr & FAT32_ATTR_DIRECTORY)
                    out[ni++] = '/';
                out[ni] = '\0';
                count++;
            }
        }
        clus = fat_next_cluster(clus);
    }
done:
    return count;
}
