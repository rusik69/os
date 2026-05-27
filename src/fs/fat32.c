/*
 * FAT32 Driver (read/write)
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
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/* ── FAT type detection and constants ───────────────────────────────────────── */
enum fat_type { FAT_TYPE_UNKNOWN = 0, FAT12, FAT16, FAT32 };

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

/* ── Driver state ───────────────────────────────────────────────────────────── */
static int          mounted       = 0;
static int          fat_type      = FAT_TYPE_UNKNOWN;
static fat32_disk_t disk_type     = FAT32_DISK_ATA;
static int          disk_id       = BLOCKDEV_ATA;
static uint32_t     part_start    = 0;   /* LBA of partition start */
static uint32_t     fat_start     = 0;   /* LBA of FAT region */
static uint32_t     data_start    = 0;   /* LBA of cluster 2 */
static uint32_t     root_cluster  = 2;
static uint32_t     root_dir_sectors = 0; /* fixed root dir size (FAT12/16 only) */
static uint32_t     spc           = 0;   /* sectors per cluster */
static uint32_t     bps           = 512; /* bytes per sector */
static uint32_t     num_fats      = 2;
static uint32_t     fat_sectors   = 0;
static uint32_t     fs_info_lba   = 0;
static uint32_t     fsinfo_next_free = 2;

/* FAT-type-aware accessors */
#define FAT_ENTRY_SIZE() (fat_type == FAT12 ? 2u : (fat_type == FAT16 ? 2u : 4u))
#define FAT_EOC()        (fat_type == FAT12 ? 0x0FF8u : (fat_type == FAT16 ? 0xFFF8u : 0x0FFFFFF8u))
#define FAT_FREE()       0u
#define FAT_MAX_CLUSTER() (fat_type == FAT12 ? 0x0FF0u : (fat_type == FAT16 ? 0xFFF0u : 0x0FFFFFF0u))

/* Sector-sized scratch buffer (on stack in helpers) */
#define SECT_SIZE 512

/* ── Low-level sector I/O ───────────────────────────────────────────────────── */
static int read_sector(uint32_t lba, void *buf) {
    return blockdev_read_sectors(disk_id, lba, 1, buf);
}

static int write_sector(uint32_t lba, const void *buf) {
    return blockdev_write_sectors(disk_id, lba, 1, buf);
}

static void fsinfo_write_hint(uint32_t next) {
    if (!fs_info_lba) return;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fs_info_lba, buf) != 0) return;
    buf[488] = (uint8_t)(next & 0xFF);
    buf[489] = (uint8_t)((next >> 8) & 0xFF);
    buf[490] = (uint8_t)((next >> 16) & 0xFF);
    buf[491] = (uint8_t)((next >> 24) & 0xFF);
    write_sector(fs_info_lba, buf);
}

/* ── FAT chain traversal (FAT12/16/32 aware) ────────────────────────────────── */

/* Read a FAT entry — automatically handles 12-bit, 16-bit, and 32-bit FATs */
static int fat_read_entry(uint32_t cluster, uint32_t *out) {
    uint32_t entry_sz = FAT_ENTRY_SIZE();
    uint32_t byte_off = cluster * entry_sz;
    uint32_t fat_sec  = fat_start + byte_off / SECT_SIZE;
    uint32_t offset   = byte_off % SECT_SIZE;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fat_sec, buf) != 0) return -1;

    if (fat_type == FAT12) {
        /* 12-bit entries: 2 entries packed in 3 bytes */
        uint32_t val;
        __builtin_memcpy(&val, buf + offset, 2);
        if (cluster & 1)
            *out = val >> 4;         /* odd cluster: high 12 bits */
        else
            *out = val & 0x0FFF;     /* even cluster: low 12 bits */
    } else if (fat_type == FAT16) {
        uint16_t val;
        __builtin_memcpy(&val, buf + offset, 2);
        *out = val;
    } else {
        /* FAT32 */
        uint32_t val;
        __builtin_memcpy(&val, buf + offset, 4);
        *out = val & 0x0FFFFFFF;
    }
    return 0;
}

/* Write a FAT entry — type-aware */
static int fat_write_entry(uint32_t cluster, uint32_t value) {
    uint32_t entry_sz = FAT_ENTRY_SIZE();
    uint32_t byte_off = cluster * entry_sz;
    uint32_t fat_sec  = fat_start + byte_off / SECT_SIZE;
    uint32_t offset   = byte_off % SECT_SIZE;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fat_sec, buf) != 0) return -1;

    if (fat_type == FAT12) {
        uint16_t old;
        __builtin_memcpy(&old, buf + offset, 2);
        if (cluster & 1)
            old = (old & 0x000F) | (uint16_t)((value & 0x0FFF) << 4);
        else
            old = (old & 0xF000) | (uint16_t)(value & 0x0FFF);
        __builtin_memcpy(buf + offset, &old, 2);
    } else if (fat_type == FAT16) {
        uint16_t v16 = (uint16_t)(value & 0xFFFF);
        __builtin_memcpy(buf + offset, &v16, 2);
    } else {
        uint32_t old;
        __builtin_memcpy(&old, buf + offset, 4);
        old = (old & 0xF0000000) | (value & 0x0FFFFFFF);
        __builtin_memcpy(buf + offset, &old, 4);
    }
    if (write_sector(fat_sec, buf) != 0) return -1;
    /* Mirror to additional FATs — entry size is same for all mirrors */
    for (uint32_t f = 1; f < num_fats; f++) {
        uint32_t mir_sec = fat_start + f * fat_sectors + byte_off / SECT_SIZE;
        if (write_sector(mir_sec, buf) != 0) return -1;
    }
    return 0;
}

static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t val = FAT_EOC();
    fat_read_entry(cluster, &val);
    return val;
}

static uint32_t fat_alloc_cluster(void) {
    uint32_t max = FAT_MAX_CLUSTER();
    uint32_t eoc = FAT_EOC();
    uint32_t start = fsinfo_next_free >= 2 ? fsinfo_next_free : 2;
    for (uint32_t pass = 0; pass < max; pass++) {
        uint32_t c = (start + pass) % max;
        if (c < 2) c = 2;
        uint32_t val;
        if (fat_read_entry(c, &val) != 0) return 0;
        if (val == FAT_FREE()) {
            if (fat_write_entry(c, eoc) != 0) return 0;
            fsinfo_next_free = c + 1;
            fsinfo_write_hint(fsinfo_next_free);
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t cluster) {
    uint32_t max_clusters = 0;
    uint32_t eoc = FAT_EOC();
    if (fat_sectors) max_clusters = (fat_sectors * SECT_SIZE * 2) / 3;
    if (!max_clusters) max_clusters = 65536;
    uint32_t visited = 0;
    while (cluster >= 2 && cluster < eoc) {
        if (++visited > max_clusters) break;
        uint32_t next = fat_next_cluster(cluster);
        fat_write_entry(cluster, FAT_FREE());
        cluster = next;
    }
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

static int name_match_ci(const char *a, const char *b) {
    for (int j = 0; a[j] || b[j]; j++) {
        char x = a[j], y = b[j];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static int lfn_build_name(const struct fat32_lfn *entries, int count, char *out, int out_max) {
    int pos = 0;
    for (int seq = count; seq >= 1; seq--) {
        const struct fat32_lfn *e = &entries[seq - 1];
        for (int i = 0; i < 5 && pos < out_max - 1; i++) {
            uint16_t c = e->name1[i];
            if (!c || c == 0xFFFF) continue;
            out[pos++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        for (int i = 0; i < 6 && pos < out_max - 1; i++) {
            uint16_t c = e->name2[i];
            if (!c || c == 0xFFFF) continue;
            out[pos++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
        for (int i = 0; i < 2 && pos < out_max - 1; i++) {
            uint16_t c = e->name3[i];
            if (!c || c == 0xFFFF) continue;
            out[pos++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        }
    }
    out[pos] = '\0';
    return pos;
}

static int dir_grow_cluster(uint32_t *cluster) {
    uint32_t newc = fat_alloc_cluster();
    if (!newc) return -1;
    fat_write_entry(*cluster, newc);
    fat_write_entry(newc, FAT_EOC());    uint32_t lba = cluster_to_lba(newc);
    uint8_t zbuf[SECT_SIZE];
    memset(zbuf, 0, SECT_SIZE);
    for (uint32_t s = 0; s < spc; s++)
        if (write_sector(lba + s, zbuf) != 0) return -1;
    *cluster = newc;
    return 0;
}

/* ── Traverse a directory cluster chain looking for a name component ─────────
 * Returns first cluster of the found entry, or 0.
 * For FAT12/16 root dir (dir_cluster == 0), reads the fixed root directory region.
 * Sets *is_dir if the entry is a directory.
 * Sets *file_size to the file size.
 */
static uint32_t dir_find(uint32_t dir_cluster, const char *name,
                          int *is_dir, uint32_t *file_size) {
    uint8_t buf[SECT_SIZE];
    uint32_t eoc = FAT_EOC();
    uint32_t sec_count;
    uint32_t first_lba;

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory */
        first_lba = fat_start + num_fats * fat_sectors;
        sec_count = root_dir_sectors;
        /* No cluster chain to follow — single layer of sectors */
        for (uint32_t s = 0; s < sec_count; s++) {
            if (read_sector(first_lba + s, buf) != 0) return 0;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            int lfn_n = 0;
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return 0;
                if (first == 0xE5) { lfn_n = 0; continue; }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        lfn_parts[ord - 1] = *(struct fat32_lfn *)&entries[i];
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) { lfn_n = 0; continue; }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    lfn_build_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                lfn_n = 0;
                if (matched) {
                    uint32_t clus = ((uint32_t)entries[i].cluster_hi << 16)
                                  | entries[i].cluster_lo;
                    if (is_dir)    *is_dir    = !!(entries[i].attr & FAT32_ATTR_DIRECTORY);
                    if (file_size) *file_size = entries[i].file_size;
                    /* For FAT12/16, cluster 0 means empty; return it anyway as the caller
                     * distinguishes by is_dir/file_size. */
                    return clus ? clus : root_cluster;
                }
            }
        }
        return 0;
    }

    /* Normal cluster-chain directory (FAT32 or FAT12/16 subdirectory) */
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < eoc) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0) return 0;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            int lfn_n = 0;
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return 0;
                if (first == 0xE5) { lfn_n = 0; continue; }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        lfn_parts[ord - 1] = *(struct fat32_lfn *)&entries[i];
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) { lfn_n = 0; continue; }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    lfn_build_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                lfn_n = 0;
                if (matched) {
                    uint32_t clus = ((uint32_t)entries[i].cluster_hi << 16)
                                  | entries[i].cluster_lo;
                    if (is_dir)    *is_dir    = !!(entries[i].attr & FAT32_ATTR_DIRECTORY);
                    if (file_size) *file_size = entries[i].file_size;
                    return clus ? clus : root_cluster;
                }
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

/* ── Resolve a path → starting cluster of the last component ────────────────── */
static uint32_t path_resolve(const char *path, int *is_dir, uint32_t *file_size) {
    while (*path == '/') path++;
    uint32_t cluster;
    if (root_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16: root is a fixed region; use 0 as sentinel for dir_find */
        cluster = 0;
    } else {
        cluster = root_cluster;
    }
    if (*path == '\0') {
        if (is_dir)    *is_dir    = 1;
        if (file_size) *file_size = 0;
        return cluster;
    }
    while (*path) {
        if (is_dir)    *is_dir    = 1;
        if (file_size) *file_size = 0;
        return cluster;
    }

    while (*path) {
        /* Extract next component */
        char comp[FAT32_MAX_NAME];
        int  ci = 0;
        while (*path && *path != '/' && ci < FAT32_MAX_NAME - 1) comp[ci++] = *path++;
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
    fat_type  = FAT_TYPE_UNKNOWN;

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
    /* Auto-detect: if part_lba == 0, read MBR and find any FAT partition */
    if (part_lba == 0) {
        if (read_sector(0, mbr) != 0) return -1;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -2;
        uint8_t *pt = mbr + 0x1BE;
        for (int i = 0; i < 4; i++) {
            uint8_t ptype = pt[i * 16 + 4];
            /* Accept FAT12 (0x01), FAT16 (0x04/0x06/0x0E), FAT32 (0x0B/0x0C) */
            if (ptype == 0x01 || ptype == 0x04 || ptype == 0x06 ||
                ptype == 0x0B || ptype == 0x0C || ptype == 0x0E) {
                uint32_t start;
                __builtin_memcpy(&start, pt + i * 16 + 8, 4);
                part_lba = start;
                break;
            }
        }
        if (part_lba == 0) return -3;  /* no FAT partition found */
    }

    /* Read BPB */
    uint8_t boot[SECT_SIZE];
    if (read_sector(part_lba, boot) != 0) return -4;

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot;

    bps = bpb->bytes_per_sector;
    spc = bpb->sectors_per_cluster;
    num_fats = bpb->num_fats ? bpb->num_fats : 2;
    uint32_t root_entries = bpb->root_entry_count;
    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    fat_start = part_lba + bpb->reserved_sectors;

    /* Determine FAT type from cluster count */
    uint32_t root_dir_sz = root_entries * 32; /* bytes */
    uint32_t root_dir_sec = (root_dir_sz + bps - 1) / bps;
    uint32_t fat_sz = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    fat_sectors = fat_sz;

    uint32_t data_sec = total_sectors
                      - bpb->reserved_sectors
                      - num_fats * fat_sz
                      - root_dir_sec;
    uint32_t total_clusters = data_sec / spc;

    if (total_clusters < 4085) {
        fat_type = FAT12;
    } else if (total_clusters < 65525) {
        fat_type = FAT16;
    } else {
        fat_type = FAT32;
    }

    /* Accept FAT32 even if calculated differently (backward compat) */
    if (fat_type != FAT32 &&
        (__builtin_memcmp(bpb->fs_type, "FAT32   ", 8) == 0 ||
         __builtin_memcmp(bpb->fs_type, "FAT32", 5) == 0))
        fat_type = FAT32;

    /* Reject unknown types */
    if (fat_type == FAT_TYPE_UNKNOWN) return -5;

    if (fat_type == FAT32) {
        root_cluster = bpb->root_cluster;
        data_start = fat_start + num_fats * fat_sz;
        root_dir_sectors = 0;
        fs_info_lba = bpb->fs_info ? part_lba + bpb->fs_info : 0;
        if (fs_info_lba) {
            uint8_t fbuf[SECT_SIZE];
            if (read_sector(fs_info_lba, fbuf) == 0) {
                fsinfo_next_free = (uint32_t)fbuf[488]
                                 | ((uint32_t)fbuf[489] << 8)
                                 | ((uint32_t)fbuf[490] << 16)
                                 | ((uint32_t)fbuf[491] << 24);
                if (fsinfo_next_free < 2) fsinfo_next_free = 2;
            }
        }
    } else {
        /* FAT12 / FAT16: fixed root directory */
        uint32_t total_root_bytes = root_entries * 32;
        root_dir_sectors = (total_root_bytes + bps - 1) / bps;
        data_start = fat_start + num_fats * fat_sz + root_dir_sectors;
        root_cluster = 0; /* 0 means "use fixed root dir" */
        fs_info_lba = 0;  /* no FSInfo sector */
    }

    part_start = part_lba;
    mounted = 1;

    const char *type_name = fat_type == FAT12 ? "FAT12" :
                            fat_type == FAT16 ? "FAT16" : "FAT32";
    kprintf("  %s mounted: vol='%.11s' clusters=%u\n",
            type_name, bpb->volume_label, (uint64_t)total_clusters);
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

    while (clus < FAT_EOC() && done < to_read) {
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

    if (cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory */
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors && count < max; s++) {
            if (read_sector(first_lba + s, buf) != 0) return count;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            int lfn_n = 0;
            for (int i = 0; i < n_entries && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return count;
                if (first == 0xE5) { lfn_n = 0; continue; }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        lfn_parts[ord - 1] = *(struct fat32_lfn *)&entries[i];
                    if (entries[i].name[0] & 0x40) lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) { lfn_n = 0; continue; }
                char *out = names[count];
                if (lfn_n > 0) {
                    lfn_build_name(lfn_parts, lfn_n, out, FAT32_MAX_NAME);
                    lfn_n = 0;
                } else {
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
                    out[ni] = '\0';
                }
                if (entries[i].attr & FAT32_ATTR_DIRECTORY)
                    strcat(out, "/");
                count++;
            }
        }
        return count;
    }

    /* Normal cluster-chain directory */
    uint32_t eoc = FAT_EOC();
    uint32_t clus = cluster;
    while (clus >= 2 && clus < eoc && count < max) {
        uint32_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && count < max; s++) {
            if (read_sector(lba + s, buf) != 0) return count;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            int lfn_n = 0;
            for (int i = 0; i < n_entries && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) goto done;
                if (first == 0xE5) { lfn_n = 0; continue; }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        lfn_parts[ord - 1] = *(struct fat32_lfn *)&entries[i];
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) { lfn_n = 0; continue; }

                char *out = names[count];
                if (lfn_n > 0) {
                    lfn_build_name(lfn_parts, lfn_n, out, FAT32_MAX_NAME);
                    lfn_n = 0;
                } else {
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
                    out[ni] = '\0';
                }
                if (entries[i].attr & FAT32_ATTR_DIRECTORY)
                    strcat(out, "/");
                count++;
            }
        }
        clus = fat_next_cluster(clus);
    }
done:
    return count;
}

/* ── Write support ──────────────────────────────────────────────────────────── */

static void name_to_83(const char *name, char out_name[8], char out_ext[3]) {
    memset(out_name, ' ', 8);
    memset(out_ext, ' ', 3);
    const char *dot = strchr(name, '.');
    int nlen = dot ? (int)(dot - name) : (int)strlen(name);
    if (nlen > 8) nlen = 8;
    for (int i = 0; i < nlen; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out_name[i] = c;
    }
    if (dot && dot[1]) {
        const char *e = dot + 1;
        int elen = (int)strlen(e);
        if (elen > 3) elen = 3;
        for (int i = 0; i < elen; i++) {
            char c = e[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            out_ext[i] = c;
        }
    }
}

/* Find directory cluster and parent path for last component */
static uint32_t path_parent_cluster(const char *path, char *leaf, int leaf_max) {
    while (*path == '/') path++;
    char tmp[FAT32_MAX_NAME];
    strncpy(tmp, path, FAT32_MAX_NAME - 1);
    tmp[FAT32_MAX_NAME - 1] = '\0';
    char *last = tmp;
    for (char *p = tmp; *p; p++)
        if (*p == '/') last = p + 1;
    strncpy(leaf, last, leaf_max - 1);
    leaf[leaf_max - 1] = '\0';
    if (last > tmp) *(last - 1) = '\0';
    else tmp[0] = '\0';
    int is_dir = 0;
    uint32_t fs = 0;
    if (tmp[0] == '\0') return root_cluster ? root_cluster : 0;
    return path_resolve(tmp, &is_dir, &fs);
}

static int dir_add_entry(uint32_t dir_cluster, const char *name83_8, const char *name83_3,
                         uint32_t first_cluster, uint32_t file_size, int is_dir) {
    uint8_t buf[SECT_SIZE];
    uint32_t eoc = FAT_EOC();

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory — search sectors directly, no cluster chain */
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0) return -1;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00 || first == 0xE5) {
                    memset(&entries[i], 0, sizeof(entries[i]));
                    memcpy(entries[i].name, name83_8, 8);
                    memcpy(entries[i].ext, name83_3, 3);
                    entries[i].attr = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    if (write_sector(first_lba + s, buf) != 0) return -1;
                    return 0;
                }
            }
        }
        return -2; /* Root directory full */
    }

    /* Normal cluster-chain directory */
    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < eoc) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0) return -1;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00 || first == 0xE5) {
                    memset(&entries[i], 0, sizeof(entries[i]));
                    memcpy(entries[i].name, name83_8, 8);
                    memcpy(entries[i].ext, name83_3, 3);
                    entries[i].attr = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    if (write_sector(lba + s, buf) != 0) return -1;
                    return 0;
                }
            }
        }
        uint32_t next = fat_next_cluster(cluster);
        if (next >= eoc) {
            if (dir_grow_cluster(&cluster) != 0) return -2;
            continue;
        }
        cluster = next;
    }
    return -2; /* directory full */
}

static int dir_update_size(uint32_t dir_cluster, const char *cmp_name,
                           uint32_t first_cluster, uint32_t file_size) {
    uint8_t buf[SECT_SIZE];
    uint32_t eoc = FAT_EOC();

    /* Inline search function used for both fixed-root and cluster-chain dirs.
     * We search two possible ranges: the fixed root dir (FAT12/16) first,
     * then fail over to the cluster chain if applicable. */

    if (dir_cluster == 0 && fat_type != FAT32) {
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0) return -1;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                if (entries[i].attr == FAT32_ATTR_LFN) continue;
                if (!name83_match(entries[i].name, entries[i].ext, cmp_name)) continue;
                entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                entries[i].file_size = file_size;
                return write_sector(first_lba + s, buf);
            }
        }
        return -1;
    }

    uint32_t cluster = dir_cluster;
    while (cluster >= 2 && cluster < eoc) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0) return -1;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                if (entries[i].attr == FAT32_ATTR_LFN) continue;
                if (!name83_match(entries[i].name, entries[i].ext, cmp_name)) continue;
                entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                entries[i].file_size = file_size;
                return write_sector(lba + s, buf);
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -1;
}

int fat32_sync(void) {
    if (!mounted) return -1;
    fsinfo_write_hint(fsinfo_next_free);
    if (fs_info_lba) {
        uint8_t buf[SECT_SIZE];
        if (read_sector(fs_info_lba, buf) != 0) return -1;
        if (write_sector(fs_info_lba, buf) != 0) return -1;
    }
    return 0;
}

int fat32_write_file(const char *path, const void *data, uint32_t size) {
    if (!mounted || !path || !*path) return -1;

    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent) return -2;

    char n8[8], n3[3];
    name_to_83(leaf, n8, n3);

    /* Build compare name for lookup */
    char cmp[13];
    int ni = 0;
    for (int i = 0; i < 8 && n8[i] != ' '; i++) cmp[ni++] = (char)((n8[i] >= 'A' && n8[i] <= 'Z') ? n8[i] + 32 : n8[i]);
    if (n3[0] != ' ') {
        cmp[ni++] = '.';
        for (int i = 0; i < 3 && n3[i] != ' '; i++)
            cmp[ni++] = (char)((n3[i] >= 'A' && n3[i] <= 'Z') ? n3[i] + 32 : n3[i]);
    }
    cmp[ni] = '\0';

    int is_dir = 0;
    uint32_t old_size = 0;
    uint32_t old_clus = dir_find(parent, cmp, &is_dir, &old_size);
    if (old_clus && !is_dir)
        fat_free_chain(old_clus);

    /* Allocate cluster chain */
    uint32_t bytes_per_cluster = spc * bps;
    uint32_t needed = size ? size : 1;
    uint32_t clusters_needed = (needed + bytes_per_cluster - 1) / bytes_per_cluster;
    if (clusters_needed == 0) clusters_needed = 1;

    uint32_t first = 0, prev = 0;
    for (uint32_t i = 0; i < clusters_needed; i++) {
        uint32_t c = fat_alloc_cluster();
        if (!c) {
            if (first) fat_free_chain(first);
            return -3;
        }
        if (!first) first = c;
        if (prev) fat_write_entry(prev, c);
        prev = c;
    }
    if (prev) fat_write_entry(prev, FAT_EOC());

    /* Write data */
    uint32_t done = 0;
    uint32_t clus = first;
    uint8_t sect_buf[SECT_SIZE];
    while (clus < FAT_EOC() && done < size) {
        uint32_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && done < size; s++) {
            memset(sect_buf, 0, SECT_SIZE);
            uint32_t chunk = SECT_SIZE;
            if (chunk > size - done) chunk = size - done;
            memcpy(sect_buf, (const uint8_t *)data + done, chunk);
            if (write_sector(lba + s, sect_buf) != 0) return (int)done;
            done += chunk;
        }
        clus = fat_next_cluster(clus);
    }

    if (!old_clus) {
        if (dir_add_entry(parent, n8, n3, first, size, 0) != 0) {
            if (first) fat_free_chain(first);
            return -4;
        }
    } else {
        dir_update_size(parent, cmp, first, size);
    }
    return (int)size;
}

static int dir_remove_entry(uint32_t dir_cluster, const char *name) {
    uint8_t buf[SECT_SIZE];
    uint32_t cluster = dir_cluster;
    while (cluster < FAT_EOC()) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0) return -1;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            int lfn_n = 0;
            int lfn_start = -1;
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00) return -1;
                if (first == 0xE5) { lfn_n = 0; lfn_start = -1; continue; }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        lfn_parts[ord - 1] = *(struct fat32_lfn *)&entries[i];
                    if (lfn_start < 0) lfn_start = i;
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) { lfn_n = 0; continue; }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    lfn_build_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                if (matched) {
                    if (lfn_start >= 0) {
                        for (int j = lfn_start; j < i; j++)
                            entries[j].name[0] = 0xE5;
                    }
                    entries[i].name[0] = 0xE5;
                    return write_sector(lba + s, buf);
                }
                lfn_n = 0;
                lfn_start = -1;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -1;
}

int fat32_mkdir(const char *path) {
    if (!mounted || !path || !*path) return -1;
    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent) return -2;
    if (dir_find(parent, leaf, 0, 0)) return -3;

    char n8[8], n3[3];
    name_to_83(leaf, n8, n3);
    uint32_t dir_clus = fat_alloc_cluster();
    if (!dir_clus) return -4;
    uint32_t lba = cluster_to_lba(dir_clus);
    uint8_t zbuf[SECT_SIZE];
    memset(zbuf, 0, SECT_SIZE);
    for (uint32_t s = 0; s < spc; s++) {
        if (write_sector(lba + s, zbuf) != 0) { fat_free_chain(dir_clus); return -5; }
    }
    /* Create "." and ".." entries in the new directory (required by FAT32) */
    char dot8[8] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    char dot38[3] = {' ', ' ', ' '};
    char dot8_2[8] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' '};
    if (dir_add_entry(dir_clus, dot8, dot38, dir_clus, 0, 1) != 0) { fat_free_chain(dir_clus); return -5; }
    if (dir_add_entry(dir_clus, dot8_2, dot38, parent, 0, 1) != 0) { fat_free_chain(dir_clus); return -5; }
    if (dir_add_entry(parent, n8, n3, dir_clus, 0, 1) != 0) { fat_free_chain(dir_clus); return -6; }
    return 0;
}

int fat32_unlink(const char *path) {
    if (!mounted || !path || !*path) return -1;
    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent) return -2;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t clus = dir_find(parent, leaf, &is_dir, &fsize);
    if (!clus) return -3;
    if (is_dir) return -4;
    if (dir_remove_entry(parent, leaf) != 0) return -5;
    fat_free_chain(clus);
    return 0;
}

/* ── VFS backend at /mnt ───────────────────────────────────────────────────── */

static const char *fat32_vfs_rel(const char *path) {
    if (strncmp(path, "/mnt", 4) == 0) {
        if (path[4] == '\0') return "/";
        if (path[4] == '/') return path + 4;
    }
    return path;
}

static int fat32_vfs_read(void *priv, const char *path, void *buf,
                          uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    if (!mounted) return -1;
    int n = fat32_read_file(fat32_vfs_rel(path), buf, max_size);
    if (n < 0) return -1;
    if (out_size) *out_size = (uint32_t)n;
    return 0;
}

static int fat32_vfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    if (!mounted) return -1;
    return fat32_write_file(fat32_vfs_rel(path), data, size) < 0 ? -1 : 0;
}

static int fat32_vfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    if (!mounted) return -1;
    int sz = fat32_file_size(fat32_vfs_rel(path));
    if (sz < 0) return -1;
    st->size = (uint32_t)sz;
    st->type = 1;
    st->uid = st->gid = 0;
    st->mode = 0644;
    st->mtime = 0;
    return 0;
}

static int fat32_vfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv;
    if (!mounted) return -1;
    if (type == 2) return fat32_mkdir(fat32_vfs_rel(path));
    return fat32_write_file(fat32_vfs_rel(path), "", 0) < 0 ? -1 : 0;
}

static int fat32_vfs_unlink(void *priv, const char *path) {
    (void)priv;
    if (!mounted) return -1;
    return fat32_unlink(fat32_vfs_rel(path));
}

static int fat32_vfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (!mounted) return -1;
    char (*names)[FAT32_MAX_NAME] = kmalloc((size_t)64 * FAT32_MAX_NAME);
    if (!names) return -1;
    int n = fat32_list_dir(fat32_vfs_rel(path), names, 64);
    for (int i = 0; i < n; i++)
        kprintf("%s\n", names[i]);
    kfree(names);
    return 0;
}

struct vfs_ops fat32_vfs_ops = {
    .read    = fat32_vfs_read,
    .write   = fat32_vfs_write,
    .stat    = fat32_vfs_stat,
    .create  = fat32_vfs_create,
    .unlink  = fat32_vfs_unlink,
    .readdir = fat32_vfs_readdir,
};
