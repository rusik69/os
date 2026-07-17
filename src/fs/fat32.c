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
#include "bufcache.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "vfs.h"

#ifdef MODULE
#include "ahci.h"
#include "ata.h"
#include "module.h"
#endif

/* ── FAT type detection and constants ───────────────────────────────────────── */
enum fat_type { FAT_TYPE_UNKNOWN = 0, FAT12, FAT16, FAT32 };

/* ── FAT32 on-disk structures ───────────────────────────────────────────────── */
struct fat32_bpb {
    uint8_t jump[3];
    char oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t num_fats;
    uint16_t root_entry_count; /* 0 for FAT32 */
    uint16_t total_sectors_16; /* 0 for FAT32 */
    uint8_t media_type;
    uint16_t fat_size_16; /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster; /* first cluster of root directory */
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    char volume_label[11];
    char fs_type[8]; /* "FAT32   " */
} __attribute__((packed));

struct fat32_dirent {
    char name[8];
    char ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t ctime_tenth;
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
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr; /* must be 0x0F */
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t cluster; /* always 0 */
    uint16_t name3[2];
} __attribute__((packed));

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN 0x02
#define FAT32_ATTR_SYSTEM 0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE 0x20
#define FAT32_ATTR_LFN 0x0F /* all of RO|HIDDEN|SYSTEM|VOLUME_ID */

/* Byte offsets within a 32-byte directory entry for LFN fields.
 * These mirror the struct fat32_lfn layout but accessed via raw bytes
 * to avoid struct-pointer aliasing violations. */
#define LFN_ORD_OFF 0
#define LFN_NAME1_OFF 1 /* 5 chars, 10 bytes */
#define LFN_ATTR_OFF 11
#define LFN_TYPE_OFF 12
#define LFN_CHKSUM_OFF 13
#define LFN_NAME2_OFF 14 /* 6 chars, 12 bytes */
#define LFN_NAME3_OFF 28 /* 2 chars, 4 bytes  */

/* ── Driver state ───────────────────────────────────────────────────────────── */
static int mounted = 0;
static int fat_type = FAT_TYPE_UNKNOWN;
static fat32_disk_t disk_type = FAT32_DISK_ATA;
static int disk_id = BLOCKDEV_ATA;
static uint32_t part_start = 0; /* LBA of partition start */
static uint32_t fat_start = 0;  /* LBA of FAT region */
static uint32_t data_start = 0; /* LBA of cluster 2 */
static uint32_t root_cluster = 2;
static uint32_t root_dir_sectors = 0; /* fixed root dir size (FAT12/16 only) */
static uint32_t spc = 0;              /* sectors per cluster */
static uint32_t bps = 512;            /* bytes per sector */
static uint32_t num_fats = 2;
static uint32_t fat_sectors = 0;
static uint32_t fs_info_lba = 0;
static uint32_t fsinfo_next_free = 2;
static uint32_t fsinfo_free_count = 0;
static uint16_t g_ext_flags = 0;

/* Cached volume label (11-char padded, null-terminated) */
static char g_volume_label[12];

/* FAT-type-aware accessors */
#define FAT_ENTRY_SIZE() (fat_type == FAT12 ? 2u : (fat_type == FAT16 ? 2u : 4u))
#define FAT_EOC() (fat_type == FAT12 ? 0x0FF8u : (fat_type == FAT16 ? 0xFFF8u : 0x0FFFFFF8u))
#define FAT_FREE() 0u
#define FAT_MAX_CLUSTER() \
    (fat_type == FAT12 ? 0x0FF0u : (fat_type == FAT16 ? 0xFFF0u : 0x0FFFFFF0u))
#define FAT_IS_EOC(val) ((val) >= FAT_EOC())

/* Sector-sized scratch buffer (on stack in helpers) */
#define SECT_SIZE 512

/* Low-level sector I/O (with buffer cache) */
static int read_sector(uint64_t lba, void *buf) {
    void *cached = bufcache_read(lba, (uint8_t)disk_id);
    if (cached) {
        memcpy(buf, cached, SECT_SIZE);
        return 0;
    }
    return blockdev_read_sectors(disk_id, lba, 1, buf);
}

static int write_sector(uint64_t lba, const void *buf) {
    return bufcache_write(lba, (uint8_t)disk_id, buf);
}

static void fsinfo_write_hint(uint32_t next) {
    if (!fs_info_lba)
        return;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fs_info_lba, buf) != 0)
        return;
    /* FSI_Free_Count at offset 488-491 (per Microsoft FAT32 spec) */
    buf[488] = (uint8_t)(fsinfo_free_count & 0xFF);
    buf[489] = (uint8_t)((fsinfo_free_count >> 8) & 0xFF);
    buf[490] = (uint8_t)((fsinfo_free_count >> 16) & 0xFF);
    buf[491] = (uint8_t)((fsinfo_free_count >> 24) & 0xFF);
    /* FSI_Nxt_Free at offset 492-495 (per Microsoft FAT32 spec) */
    buf[492] = (uint8_t)(next & 0xFF);
    buf[493] = (uint8_t)((next >> 8) & 0xFF);
    buf[494] = (uint8_t)((next >> 16) & 0xFF);
    buf[495] = (uint8_t)((next >> 24) & 0xFF);
    write_sector(fs_info_lba, buf);
}

/* ── FAT chain traversal (FAT12/16/32 aware) ────────────────────────────────── */

/*
 * Return the LBA base address of the active FAT, taking BPB_ExtFlags into
 * account.  Per the FAT32 spec:
 *   - Bit 7 = 0 : all FATs are mirrored; use FAT 0 as the primary.
 *   - Bit 7 = 1 : only one FAT is active; bits 0-3 select which.
 */
static uint32_t fat_active_base(void) {
    if (g_ext_flags & 0x80) {
        uint32_t active = g_ext_flags & 0x0F;
        /* Clamp to a valid FAT index — fall back to 0 if the value is bogus */
        if (active >= num_fats)
            active = 0;
        return fat_start + active * fat_sectors;
    }
    return fat_start;
}

/* Read a FAT entry — automatically handles 12-bit, 16-bit, and 32-bit FATs */
static int fat_read_entry(uint32_t cluster, uint32_t *out) {
    uint32_t entry_sz = FAT_ENTRY_SIZE();
    uint32_t byte_off = cluster * entry_sz;
    uint32_t fat_base = fat_active_base();
    uint32_t fat_sec = fat_base + byte_off / SECT_SIZE;
    uint32_t offset = byte_off % SECT_SIZE;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fat_sec, buf) != 0)
        return -EIO;

    if (fat_type == FAT12) {
        /* 12-bit entries: 2 entries packed in 3 bytes */
        uint32_t val;
        __builtin_memcpy(&val, buf + offset, 2);
        if (cluster & 1)
            *out = val >> 4; /* odd cluster: high 12 bits */
        else
            *out = val & 0x0FFF; /* even cluster: low 12 bits */
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
    uint32_t fat_base = fat_active_base();
    uint32_t fat_sec = fat_base + byte_off / SECT_SIZE;
    uint32_t offset = byte_off % SECT_SIZE;
    uint8_t buf[SECT_SIZE];
    if (read_sector(fat_sec, buf) != 0)
        return -EIO;

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
    if (write_sector(fat_sec, buf) != 0)
        return -EIO;
    /* Mirror to additional FATs unless ext_flags bit 7 disables mirroring */
    if (!(g_ext_flags & 0x80)) {
        for (uint32_t f = 1; f < num_fats; f++) {
            uint32_t mir_sec = fat_start + f * fat_sectors + byte_off / SECT_SIZE;
            if (write_sector(mir_sec, buf) != 0)
                return -EIO;
        }
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
        if (c < 2)
            c = 2;
        uint32_t val;
        if (fat_read_entry(c, &val) != 0)
            return 0;
        if (val == FAT_FREE()) {
            if (fat_write_entry(c, eoc) != 0)
                return 0;
            fsinfo_next_free = c + 1;
            if (fsinfo_free_count > 0)
                fsinfo_free_count--;
            fsinfo_write_hint(fsinfo_next_free);
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(uint32_t cluster) {
    uint32_t max_clusters = 0;
    if (fat_sectors) {
        /* FAT-type-aware entry count: FAT12=1.5, FAT16=2, FAT32=4 bytes/entry */
        uint32_t bytes_per_entry = (fat_type == FAT12) ? 3u : (fat_type == FAT16 ? 2u : 4u);
        max_clusters = (fat_sectors * (uint64_t)SECT_SIZE) / bytes_per_entry;
    }
    if (!max_clusters)
        max_clusters = 65536;
    uint32_t visited = 0;
    uint32_t first_freed = cluster;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++visited > max_clusters)
            break;
        uint32_t next = fat_next_cluster(cluster);
        fat_write_entry(cluster, FAT_FREE());
        fsinfo_free_count++;
        cluster = next;
    }
    /* Update next-free hint if we freed clusters before the current hint */
    if (first_freed >= 2 && first_freed < fsinfo_next_free) {
        fsinfo_next_free = first_freed;
    }
    fsinfo_write_hint(fsinfo_next_free);
}

/* Cluster number → LBA of first sector */
static uint64_t cluster_to_lba(uint32_t cluster) {
    return (uint64_t)data_start + (uint64_t)(cluster - 2) * (uint64_t)spc;
}

/* ── 8.3 name comparison (case-insensitive) ─────────────────────────────────── */
static int name83_match(const char name8[8], const char ext3[3], const char *want) {
    /* Build "NAME.EXT" or "NAME" */
    char buf[13];
    int ni = 0;
    for (int i = 0; i < 8 && name8[i] != ' '; i++) {
        char c = name8[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        buf[ni++] = c;
    }
    if (ext3[0] != ' ') {
        buf[ni++] = '.';
        for (int i = 0; i < 3 && ext3[i] != ' '; i++) {
            char c = ext3[i];
            if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
            buf[ni++] = c;
        }
    }
    buf[ni] = '\0';

    /* Compare case-insensitively */
    int j = 0;
    for (; buf[j] && want[j]; j++) {
        char a = buf[j], b = want[j];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z')
            b = (char)(b + 32);
        if (a != b)
            return 0;
    }
    return buf[j] == '\0' && want[j] == '\0';
}

static int name_match_ci(const char *a, const char *b) {
    for (int j = 0; a[j] || b[j]; j++) {
        char x = a[j], y = b[j];
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return 1;
}

/* Compute the 13-char checksum over an 8.3 name (11 bytes: 8 name + 3 ext) */
static uint8_t lfn_checksum(const char *name83_8, const char *name83_3);

/* Validate that the LFN entries' checksum matches the 8.3 entry.
 * Returns 1 if checksums match (or if no LFN entries), 0 on mismatch. */
static int lfn_validate_checksum(const struct fat32_lfn *entries, int count, const char *name83_8,
                                 const char *name83_3) {
    if (count <= 0)
        return 1;
    uint8_t computed_cksum = lfn_checksum(name83_8, name83_3);
    /* Verify all LFN entries have the same checksum matching the short name */
    for (int i = 0; i < count; i++) {
        if (entries[i].checksum != computed_cksum)
            return 0;
    }
    return 1;
}

static int dir_grow_cluster(uint32_t *cluster) {
    uint32_t newc = fat_alloc_cluster();
    if (!newc)
        return -EINVAL;
    fat_write_entry(*cluster, newc);
    fat_write_entry(newc, FAT_EOC());
    uint64_t lba = cluster_to_lba(newc);
    uint8_t zbuf[SECT_SIZE];
    memset(zbuf, 0, SECT_SIZE);
    for (uint32_t s = 0; s < spc; s++)
        if (write_sector(lba + s, zbuf) != 0)
            return -EIO;
    *cluster = newc;
    return 0;
}

/* ── LFN (Long File Name) helpers for write support ───────────────────────── */

/* Compute the 13-char checksum over an 8.3 name (11 bytes: 8 name + 3 ext) */
static uint8_t lfn_checksum(const char *name83_8, const char *name83_3) {
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83_8[i]);
    for (int i = 0; i < 3; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83_3[i]);
    return sum;
}

/* Determine whether a filename needs LFN entries (doesn't fit 8.3 exactly, or has mixed case) */
static int needs_lfn(const char *leaf, const char *name83_8, const char *name83_3) {
    /* Build the canonical 8.3 representation from the leaf */
    char gen8[9], gen3[4];
    memset(gen8, ' ', 8);
    gen8[8] = '\0';
    memset(gen3, ' ', 3);
    gen3[3] = '\0';
    const char *dot = strchr(leaf, '.');
    int nlen = dot ? (int)(dot - leaf) : (int)strlen(leaf);
    if (nlen > 8)
        return 1; /* name too long for 8.3 */
    /* If name has any lowercase, needs LFN */
    for (int i = 0; leaf[i] && leaf[i] != '.'; i++)
        if (leaf[i] >= 'a' && leaf[i] <= 'z')
            return 1;
    if (dot && dot[1]) {
        int elen = (int)strlen(dot + 1);
        if (elen > 3)
            return 1; /* extension too long for 8.3 */
        for (int i = 0; dot[1 + i]; i++)
            if (dot[1 + i] >= 'a' && dot[1 + i] <= 'z')
                return 1;
    }
    /* Check if the 8.3 name matches what name_to_83 would produce — if not, it needs LFN */
    for (int i = 0; i < 8; i++) {
        char c = (i < nlen) ? leaf[i] : ' ';
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);
        if (c != name83_8[i])
            return 1;
    }
    if (dot && dot[1]) {
        for (int i = 0; i < 3; i++) {
            char c = dot[1 + i];
            if (!c)
                c = ' ';
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (c != name83_3[i])
                return 1;
        }
    }
    return 0;
}

/* Encode up to 13 UTF-16LE characters from the name starting at *pos into an LFN entry.
 * Updates *pos to point past the consumed characters.
 * Returns the number of characters written (padded with 0x0000/0xFFFF). */
static int lfn_fill_entry(struct fat32_lfn *lfn, const char *name, int *pos) {
    uint16_t buf[13];
    int written = 0;
    for (int i = 0; i < 13; i++) {
        if (name[*pos]) {
            buf[i] = (uint8_t)name[*pos]; /* ASCII only — extend to UTF-16LE */
            (*pos)++;
            written++;
        } else {
            buf[i] = (i == 0) ? 0x0000 : 0xFFFF; /* null-term, then pad with 0xFFFF */
        }
    }
    /* Copy into the three name fields */
    for (int i = 0; i < 5; i++)
        lfn->name1[i] = buf[i];
    for (int i = 0; i < 6; i++)
        lfn->name2[i] = buf[5 + i];
    for (int i = 0; i < 2; i++)
        lfn->name3[i] = buf[11 + i];
    lfn->attr = FAT32_ATTR_LFN;
    lfn->type = 0;
    lfn->cluster = 0;
    return written;
}

/* Write LFN directory entries before a new 8.3 entry.
 * The buffer 'buf' should contain the sector where the 8.3 entry will be written,
 * and 'entry_idx' is the index within that sector. Returns 0 on success, -1 on failure.
 * This function modifies 'buf' in-memory AND writes to disk the previous sector(s)
 * if the LFN entries span across sector boundaries. */
static int dir_add_lfn_entries(uint64_t lba_base, uint32_t sector_idx, int entry_idx, uint8_t *buf,
                               const char *leaf, const char *name83_8, const char *name83_3) {
    int name_len = (int)strlen(leaf);
    if (name_len <= 0)
        return -EINVAL;

    /* Count how many LFN entries we need (13 chars per entry) */
    int num_entries = (name_len + 12) / 13;
    if (num_entries > 20)
        num_entries = 20; /* max 20 LFN entries per file */

    uint8_t cksum = lfn_checksum(name83_8, name83_3);

    /* Write LFN entries in reverse order: last entry first in the directory.
     * The last entry (highest ordinal) goes closest to the beginning of the dir,
     * and is marked with bit 6 set in the order field. */
    for (int n = num_entries - 1; n >= 0; n--) {
        /* Write LFN entry starting at (sector_idx, entry_idx).
         * We need to shift the existing entries (and any future entries) down
         * to make room. We do this by rewriting the sector with the LFN entry
         * inserted before the current entry_idx. */
        struct fat32_lfn lfn_entry;
        memset(&lfn_entry, 0, sizeof(lfn_entry));

        /* Set order: first entry (ordinal 1) = 1, last entry = 0x40 | N */
        int ord = n + 1;
        if (n == num_entries - 1)
            ord |= 0x40; /* LAST_LFN bit */
        lfn_entry.order = (uint8_t)ord;

        /* Encode the name starting at position n*13 */
        int pos = n * 13;
        lfn_fill_entry(&lfn_entry, leaf, &pos);
        lfn_entry.checksum = cksum;

        /* We need to insert this LFN entry just before the current dir entry.
         * Since we're writing in reverse order (from last LFN to first),
         * we always insert at the same spot — just before the 8.3 entry.
         * Strategy: shift all entries from entry_idx onward by one slot,
         * then write the LFN at entry_idx. */
        int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
        if (entry_idx >= n_entries) {
            /* Need to flush this sector and move to the next */
            if (write_sector(lba_base + sector_idx, buf) != 0)
                return -EIO;
            sector_idx++;
            entry_idx = 0;
            memset(buf, 0, SECT_SIZE);
        }

        /* Shift existing entries down by one */
        struct fat32_dirent *dirents = (struct fat32_dirent *)buf;
        if (entry_idx < n_entries - 1) {
            memmove(&dirents[entry_idx + 1], &dirents[entry_idx],
                    (size_t)(n_entries - entry_idx - 1) * sizeof(struct fat32_dirent));
        }
        /* Write the LFN entry at entry_idx */
        memcpy(&dirents[entry_idx], &lfn_entry, sizeof(struct fat32_lfn));
        entry_idx++;
    }

    /* Write back the sector (the caller will write the 8.3 entry) */
    if (write_sector(lba_base + sector_idx, buf) != 0)
        return -EIO;
    return 0;
}

/* ── Traverse a directory cluster chain looking for a name component ─────────
 * Returns first cluster of the found entry, or 0.
 * For FAT12/16 root dir (dir_cluster == 0), reads the fixed root directory region.
 * Sets *is_dir if the entry is a directory.
 * Sets *file_size to the file size.
 */
static uint32_t dir_find(uint32_t dir_cluster, const char *name, int *is_dir, uint32_t *file_size) {
    uint8_t buf[SECT_SIZE];
    uint32_t sec_count;
    uint32_t first_lba;

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory */
        first_lba = fat_start + num_fats * fat_sectors;
        sec_count = root_dir_sectors;
        /* No cluster chain to follow — single layer of sectors */
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        for (uint32_t s = 0; s < sec_count; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                return 0;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return 0;
                if (first == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    vfat_reconstruct_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    /* Validate LFN checksum against the 8.3 entry */
                    if (!lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        lfn_n = 0;
                        continue; /* checksum mismatch — skip this entry */
                    }
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                lfn_n = 0;
                if (matched) {
                    /* Mask to 28-bit FAT32 cluster range; harmless for FAT12/16
                     * where cluster_hi is always 0 on valid volumes. */
                    uint32_t clus =
                        (((uint32_t)entries[i].cluster_hi << 16) | entries[i].cluster_lo) &
                        0x0FFFFFFF;
                    if (is_dir)
                        *is_dir = !!(entries[i].attr & FAT32_ATTR_DIRECTORY);
                    if (file_size)
                        *file_size = entries[i].file_size;
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
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return 0;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return 0;
                if (first == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    vfat_reconstruct_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    /* Validate LFN checksum against the 8.3 entry */
                    if (!lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        lfn_n = 0;
                        continue; /* checksum mismatch — skip this entry */
                    }
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                lfn_n = 0;
                if (matched) {
                    /* Mask to 28-bit FAT32 cluster range; harmless for FAT12/16
                     * where cluster_hi is always 0 on valid volumes. */
                    uint32_t clus =
                        (((uint32_t)entries[i].cluster_hi << 16) | entries[i].cluster_lo) &
                        0x0FFFFFFF;
                    if (is_dir)
                        *is_dir = !!(entries[i].attr & FAT32_ATTR_DIRECTORY);
                    if (file_size)
                        *file_size = entries[i].file_size;
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
    while (*path == '/')
        path++;
    uint32_t cluster;
    if (root_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16: root is a fixed region; use 0 as sentinel for dir_find */
        cluster = 0;
    } else {
        cluster = root_cluster;
    }
    if (*path == '\0') {
        if (is_dir)
            *is_dir = 1;
        if (file_size)
            *file_size = 0;
        return cluster;
    }

    while (*path) {
        /* Extract next component */
        char comp[FAT32_MAX_NAME];
        int ci = 0;
        while (*path && *path != '/' && ci < FAT32_MAX_NAME - 1)
            comp[ci++] = *path++;
        comp[ci] = '\0';
        while (*path == '/')
            path++;

        int d = 0;
        uint32_t fs = 0;
        uint32_t next = dir_find(cluster, comp, &d, &fs);
        if (!next)
            return 0;

        cluster = next;
        if (is_dir)
            *is_dir = d;
        if (file_size)
            *file_size = fs;

        if (*path && !d)
            return 0; /* not a directory but more path remains */
    }
    return cluster;
}

/* Forward declarations for FSInfo validation (defined later in repair section) */
static int fat32_validate_fsinfo(const uint8_t *buf);

/* ── Public API ─────────────────────────────────────────────────────────────── */
int fat32_is_mounted(void) {
    return mounted;
}

int fat32_mount(fat32_disk_t disk, uint32_t part_lba) {
    disk_type = disk;
    mounted = 0;
    fat_type = FAT_TYPE_UNKNOWN;

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
    if (!blockdev_is_registered(disk_id))
        return -10;

    /* Initialize and enable buffer cache for FAT32 sector caching */
    bufcache_init();
    bufcache_enable();

    uint8_t mbr[SECT_SIZE];
    /* Auto-detect: if part_lba == 0, read MBR and find any FAT partition */
    if (part_lba == 0) {
        if (read_sector(0, mbr) != 0)
            return -EIO;
        if (mbr[510] != 0x55 || mbr[511] != 0xAA)
            return -2;
        uint8_t *pt = mbr + 0x1BE;
        for (int i = 0; i < 4; i++) {
            uint8_t ptype = pt[i * 16 + 4];
            /* Accept FAT12 (0x01), FAT16 (0x04/0x06/0x0E), FAT32 (0x0B/0x0C) */
            if (ptype == 0x01 || ptype == 0x04 || ptype == 0x06 || ptype == 0x0B || ptype == 0x0C ||
                ptype == 0x0E) {
                uint32_t start;
                __builtin_memcpy(&start, pt + i * 16 + 8, 4);
                part_lba = start;
                break;
            }
        }
        if (part_lba == 0)
            return -3; /* no FAT partition found */
    }

    /* Read BPB */
    uint8_t boot[SECT_SIZE];
    if (read_sector(part_lba, boot) != 0)
        return -4;

    struct fat32_bpb *bpb = (struct fat32_bpb *)boot;

    bps = bpb->bytes_per_sector;
    spc = bpb->sectors_per_cluster;
    num_fats = bpb->num_fats ? bpb->num_fats : 2;
    uint32_t root_entries = bpb->root_entry_count;
    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16 : bpb->total_sectors_32;
    fat_start = part_lba + bpb->reserved_sectors;
    /* Prevent overflow: if fat_start wrapped around, the BPB or partition
     * table specifies an impossible geometry. */
    if (fat_start < part_lba)
        return -EINVAL;

    /* Validate geometry before any division operations */
    if (bps == 0)
        return -5;
    if (spc == 0)
        return -6;
    if (bpb->reserved_sectors == 0)
        return -7;
    /* Determine FAT type from cluster count */
    uint32_t root_dir_sz = root_entries * 32; /* bytes */
    uint32_t root_dir_sec = (root_dir_sz + bps - 1) / bps;
    uint32_t fat_sz = bpb->fat_size_16 ? bpb->fat_size_16 : bpb->fat_size_32;
    fat_sectors = fat_sz;

    /* Prevent arithmetic underflow — total geometry must fit within total sectors */
    if ((uint64_t)bpb->reserved_sectors + (uint64_t)num_fats * fat_sz + root_dir_sec >
        total_sectors)
        return -EINVAL;

    uint32_t data_sec = total_sectors - bpb->reserved_sectors - num_fats * fat_sz - root_dir_sec;
    uint32_t total_clusters = data_sec / spc;

    if (total_clusters < 4085) {
        fat_type = FAT12;
    } else if (total_clusters < 65525) {
        fat_type = FAT16;
    } else {
        fat_type = FAT32;
    }

    /* Accept FAT32 even if calculated differently (backward compat) */
    if (fat_type != FAT32 && (__builtin_memcmp(bpb->fs_type, "FAT32   ", 8) == 0 ||
                              __builtin_memcmp(bpb->fs_type, "FAT32", 5) == 0))
        fat_type = FAT32;

    /* Reject unknown types */
    if (fat_type == FAT_TYPE_UNKNOWN)
        return -5;

    if (fat_type == FAT32) {
        root_cluster = bpb->root_cluster;
        /* FAT32 requires at least 2 reserved sectors (boot sector + FSInfo) */
        if (bpb->reserved_sectors < 2)
            return -EINVAL;
        /* FAT32 root cluster must be a valid cluster number (>= 2) */
        if (root_cluster < 2 || FAT_IS_EOC(root_cluster))
            return -EINVAL;
        data_start = fat_start + num_fats * fat_sz;
        root_dir_sectors = 0;
        g_ext_flags = bpb->ext_flags;
        /* BPB_FSInfo (fs_info): 0xFFFF means "no FSInfo sector" per spec.
         * Also reject values outside the reserved area — a corrupted BPB should
         * not cause us to read an arbitrary sector as FSInfo. */
        if (bpb->fs_info == 0xFFFF || (bpb->fs_info != 0 && bpb->fs_info >= bpb->reserved_sectors))
            fs_info_lba = 0;
        else
            fs_info_lba = bpb->fs_info ? part_lba + bpb->fs_info : 0;
        if (fs_info_lba) {
            uint8_t fbuf[SECT_SIZE];
            if (read_sector(fs_info_lba, fbuf) == 0) {
                /* Only trust cached values if FSInfo signatures are valid */
                if (fat32_validate_fsinfo(fbuf) == 0) {
                    /* FSI_Nxt_Free at offset 492-495 */
                    fsinfo_next_free = (uint32_t)fbuf[492] | ((uint32_t)fbuf[493] << 8) |
                                       ((uint32_t)fbuf[494] << 16) | ((uint32_t)fbuf[495] << 24);
                    if (fsinfo_next_free < 2 || fsinfo_next_free == 0xFFFFFFFF)
                        fsinfo_next_free = 2;
                    else if ((uint64_t)fsinfo_next_free >= (uint64_t)total_clusters + 2)
                        fsinfo_next_free = 2;
                    /* FSI_Free_Count at offset 488-491 */
                    fsinfo_free_count = (uint32_t)fbuf[488] | ((uint32_t)fbuf[489] << 8) |
                                        ((uint32_t)fbuf[490] << 16) | ((uint32_t)fbuf[491] << 24);
                    if (fsinfo_free_count == 0xFFFFFFFF)
                        fsinfo_free_count = 0;
                    else if (fsinfo_free_count > total_clusters)
                        fsinfo_free_count = total_clusters;
                }
                /* If signatures are invalid, keep defaults (next_free=2, free_count=0) */
            }
        }
    } else {
        /* FAT12 / FAT16: fixed root directory */
        g_ext_flags = 0; /* no extended flags for FAT12/16 */
        uint32_t total_root_bytes = root_entries * 32;
        root_dir_sectors = (total_root_bytes + bps - 1) / bps;
        data_start = fat_start + num_fats * fat_sz + root_dir_sectors;
        root_cluster = 0; /* 0 means "use fixed root dir" */
        fs_info_lba = 0;  /* no FSInfo sector */
    }

    part_start = part_lba;
    mounted = 1;

    const char *type_name = fat_type == FAT12 ? "FAT12" : fat_type == FAT16 ? "FAT16" : "FAT32";
    kprintf("  %s mounted: vol='%.11s' clusters=%lu\n", type_name, bpb->volume_label,
            (unsigned long)total_clusters);
    __builtin_memcpy(g_volume_label, bpb->volume_label, 11);
    g_volume_label[11] = '\0';
    return 0;
}

int fat32_file_size(const char *path) {
    if (!mounted)
        return -EINVAL;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t clus = path_resolve(path, &is_dir, &fsize);
    if (!clus || is_dir)
        return -EINVAL;
    return (int)fsize;
}

int fat32_read_file(const char *path, void *buf, uint32_t max_size) {
    if (!mounted)
        return -EINVAL;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t cluster = path_resolve(path, &is_dir, &fsize);
    if (!cluster || is_dir)
        return -EINVAL;

    uint32_t to_read = fsize < max_size ? fsize : max_size;
    uint32_t done = 0;
    uint8_t sect_buf[SECT_SIZE];
    uint32_t clus = cluster;
    uint64_t _chain_cnt = 0;

    while (!FAT_IS_EOC(clus) && done < to_read) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            return -EIO;
        uint64_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && done < to_read; s++) {
            if (read_sector(lba + s, sect_buf) != 0)
                return (int)done;
            uint32_t chunk = SECT_SIZE;
            if (chunk > to_read - done)
                chunk = to_read - done;
            __builtin_memcpy((uint8_t *)buf + done, sect_buf, chunk);
            done += chunk;
        }
        clus = fat_next_cluster(clus);
    }
    /* File size vs cluster chain consistency: if the chain ended before we
     * read the declared file size, the filesystem is corrupted or inconsistent. */
    if (done < to_read)
        return -EIO;
    return (int)done;
}

int fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max) {
    if (!mounted)
        return -EINVAL;
    int is_dir = 0;
    uint32_t cluster = path_resolve(path, &is_dir, (void *)0);
    if (!cluster || !is_dir)
        return -EINVAL;

    int count = 0;
    uint8_t buf[SECT_SIZE];

    if (cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory */
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        for (uint32_t s = 0; s < root_dir_sectors && count < max; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                return count;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return count;
                if (first == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                char *out = names[count];
                int use_lfn = 0;
                if (lfn_n > 0) {
                    if (lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        use_lfn = 1;
                    }
                }
                if (use_lfn) {
                    vfat_reconstruct_name(lfn_parts, lfn_n, out, FAT32_MAX_NAME);
                    lfn_n = 0;
                } else {
                    int ni = 0;
                    for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                        char c = entries[i].name[j];
                        if (c >= 'A' && c <= 'Z')
                            c = (char)(c + 32);
                        out[ni++] = c;
                    }
                    if (entries[i].ext[0] != ' ') {
                        out[ni++] = '.';
                        for (int j = 0; j < 3 && entries[i].ext[j] != ' '; j++) {
                            char c = entries[i].ext[j];
                            if (c >= 'A' && c <= 'Z')
                                c = (char)(c + 32);
                            out[ni++] = c;
                        }
                    }
                    out[ni] = '\0';
                    lfn_n = 0;
                }
                if (entries[i].attr & FAT32_ATTR_DIRECTORY) {
                    size_t _slen = strlen(out);
                    if (_slen + 1 < FAT32_MAX_NAME) {
                        out[_slen] = '/';
                        out[_slen + 1] = '\0';
                    }
                }
                count++;
            }
        }
        return count;
    }

    /* Normal cluster-chain directory */
    uint32_t clus = cluster;
    uint64_t _chain_cnt = 0;
    while (clus >= 2 && !FAT_IS_EOC(clus) && count < max) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(clus);
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        for (uint32_t s = 0; s < spc && count < max; s++) {
            if (read_sector(lba + s, buf) != 0)
                return count;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries && count < max; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    goto done;
                if (first == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }

                char *out = names[count];
                int use_lfn = 0;
                if (lfn_n > 0) {
                    if (lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        use_lfn = 1;
                    }
                }
                if (use_lfn) {
                    vfat_reconstruct_name(lfn_parts, lfn_n, out, FAT32_MAX_NAME);
                    lfn_n = 0;
                } else {
                    int ni = 0;
                    for (int j = 0; j < 8 && entries[i].name[j] != ' '; j++) {
                        char c = entries[i].name[j];
                        if (c >= 'A' && c <= 'Z')
                            c = (char)(c + 32);
                        out[ni++] = c;
                    }
                    if (entries[i].ext[0] != ' ') {
                        out[ni++] = '.';
                        for (int j = 0; j < 3 && entries[i].ext[j] != ' '; j++) {
                            char c = entries[i].ext[j];
                            if (c >= 'A' && c <= 'Z')
                                c = (char)(c + 32);
                            out[ni++] = c;
                        }
                    }
                    out[ni] = '\0';
                    lfn_n = 0;
                }
                if (entries[i].attr & FAT32_ATTR_DIRECTORY) {
                    size_t _slen = strlen(out);
                    if (_slen + 1 < FAT32_MAX_NAME) {
                        out[_slen] = '/';
                        out[_slen + 1] = '\0';
                    }
                }
                count++;
            }
        }
        clus = fat_next_cluster(clus);
    }
done:
    return count;
}

/* ── Write support ──────────────────────────────────────────────────────────── */

/*
 * ── VFAT 8.3 short name generation (Item 333) ─────────────────────────
 *
 * Generates a unique MS-DOS-compatible 8.3 name from a long filename,
 * with collision resolution using numeric tails (~N).  Follows the
 * Windows VFAT algorithm:
 *   1. Strip invalid characters; uppercase allowed chars
 *   2. Truncate base name to first 6 chars, append ~N (N=1..999999)
 *   3. Truncate extension to first 3 chars (no dot in the 8.3 name)
 *   4. If collision, increment N until unique or exhausted
 */

/* Valid 8.3 characters: A-Z, 0-9, $ % ' - _ @ ~ ` ! ( ) ^ # & */
static int is_valid_83_char(char c) {
    if (c >= 'A' && c <= 'Z')
        return 1;
    if (c >= '0' && c <= '9')
        return 1;
    const char valid_special[] = "$%'-_@~`!()^#&";
    for (int i = 0; valid_special[i]; i++)
        if (c == valid_special[i])
            return 1;
    return 0;
}

/* Check if an exact 8.3 name (8+3 bytes) already exists in a directory.
 * Returns 1 if found, 0 if not found or error. */
static int fat32_83_name_exists(uint32_t dir_cluster, const char name83_8[8],
                                const char name83_3[3]) {
    uint8_t buf[SECT_SIZE];

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16: fixed root directory */
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                continue;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return 0; /* end of directory */
                if (first == 0xE5 || entries[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;
                /* Compare 8-byte name and 3-byte extension */
                if (__builtin_memcmp(entries[i].name, name83_8, 8) == 0 &&
                    __builtin_memcmp(entries[i].ext, name83_3, 3) == 0)
                    return 1;
            }
        }
        return 0;
    }

    /* FAT32 / non-root: follow cluster chain */
    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                continue;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return 0;
                if (first == 0xE5 || entries[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;
                if (__builtin_memcmp(entries[i].name, name83_8, 8) == 0 &&
                    __builtin_memcmp(entries[i].ext, name83_3, 3) == 0)
                    return 1;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

/*
 * Generate a unique 8.3 short name from a long filename.
 *
 * @long_name:  The full filename (may contain extension after last dot)
 * @dir_cluster: Cluster of the parent directory (0 for FAT12/16 root)
 * @out_name:   8-byte output buffer (space-padded, NOT null-terminated)
 * @out_ext:    3-byte extension output buffer (space-padded)
 *
 * Returns 0 on success, -1 if unable to generate a unique name.
 */
static int fat32_generate_short_name(const char *long_name, uint32_t dir_cluster, char out_name[8],
                                     char out_ext[3]) {
    char base[9]; /* 8 + NUL */
    char ext[4];  /* 3 + NUL */
    int base_len, ext_len;
    const char *dot;

    /* ---- Step 1: Split name and extension ---- */
    /* Find the LAST dot (VFAT uses last dot for extension, not first) */
    dot = NULL;
    {
        const char *p = long_name;
        while (*p) {
            if (*p == '.')
                dot = p;
            p++;
        }
    }

    if (dot && dot > long_name) {
        base_len = (int)(dot - long_name);
        /* Extension: everything after the last dot */
        const char *e = dot + 1;
        ext_len = (int)strlen(e);
        if (ext_len > 3)
            ext_len = 3;
        memcpy(ext, e, (size_t)ext_len);
        ext[ext_len] = '\0';
    } else {
        base_len = (int)strlen(long_name);
        ext_len = 0;
        ext[0] = '\0';
    }
    if (base_len > 8)
        base_len = 8;

    /* ---- Step 2: Uppercase and validate base characters ---- */
    {
        int oi = 0;
        for (int i = 0; i < base_len && oi < 8; i++) {
            char c = long_name[i];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (is_valid_83_char(c)) {
                base[oi++] = c;
            }
        }
        base[oi] = '\0';
        base_len = oi;
    }

    /* ---- Step 3: Uppercase and validate extension characters ---- */
    {
        int oi = 0;
        const char *e = (dot && dot[1]) ? dot + 1 : "";
        for (int i = 0; e[i] && oi < 3; i++) {
            char c = e[i];
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            if (is_valid_83_char(c)) {
                ext[oi++] = c;
            }
        }
        ext[oi] = '\0';
        ext_len = oi;
    }

    /* ---- Step 4: Handle special names (DOS reserved names) ---- */
    /* If base matches DOS reserved names, prefix with '_' to avoid conflicts */
    {
        const char *reserved[] = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                  "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                  "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9", NULL};
        for (int r = 0; reserved[r]; r++) {
            if (strcmp(base, reserved[r]) == 0) {
                /* Try prefixing with '_' */
                if (base_len < 8) {
                    memmove(base + 1, base, (size_t)base_len + 1);
                    base[0] = '_';
                    base_len++;
                    if (base_len > 8)
                        base_len = 8;
                    base[base_len] = '\0';
                }
                break;
            }
        }
    }

    /* ---- Step 5: Try base name as-is first, then with numeric tails ---- */
    /* VFAT algorithm: base 6 chars + ~N (N = 1..999999) */
    for (int tail = 0; tail < 1000000; tail++) {
        char try_name[8];
        char try_ext[3];

        memset(try_name, ' ', 8);
        memset(try_ext, ' ', 3);

        if (tail == 0) {
            /* Try the base name as-is */
            memcpy(try_name, base, (size_t)base_len);
        } else {
            /* Generate name with numeric tail ~N */
            /* Format: first 6-(digits_of_N) chars of base + ~N */
            /* VFAT uses: first 6 chars + ~N (if base > 6, truncate to 6) */
            char tail_str[7]; /* ~NNNNN + NUL */
            int tn = snprintf(tail_str, sizeof(tail_str), "~%d", tail);
            if (tn < 2 || tn > 7)
                continue;

            int avail = 8 - tn; /* space for base chars before the tail */
            if (avail < 0)
                continue;

            /* Take first 'avail' chars of base, then append ~N */
            int src_len = base_len < avail ? base_len : avail;
            memcpy(try_name, base, (size_t)src_len);
            memcpy(try_name + src_len, tail_str, (size_t)tn);
        }

        /* Copy extension */
        memcpy(try_ext, ext, (size_t)(ext_len < 3 ? ext_len : 3));

        /* Check for collision */
        if (!fat32_83_name_exists(dir_cluster, try_name, try_ext)) {
            /* Unique name found! Copy to output buffers */
            memcpy(out_name, try_name, 8);
            memcpy(out_ext, try_ext, 3);
            return 0;
        }

        /* If the simple base name with no extension collides, try with ~1 */
        if (tail == 0 && ext_len > 0) {
            /* First try without the extension suffix (just base name) */
        }
    }

    /* Unable to generate unique name after 1M attempts */
    kprintf("[FAT32] Warning: could not generate unique 8.3 name for '%s'\n", long_name);
    /* Fallback: use hashed name */
    {
        uint32_t hash = 0;
        const char *p = long_name;
        while (*p) {
            hash = hash * 31 + (uint8_t)(*p);
            p++;
        }
        memset(out_name, ' ', 8);
        memset(out_ext, ' ', 3);
        snprintf(out_name, 8, "~%05X", (unsigned int)(hash & 0xFFFFF));
    }
    return 0;
}

/* Find directory cluster and parent path for last component */
static uint32_t path_parent_cluster(const char *path, char *leaf, int leaf_max) {
    while (*path == '/')
        path++;
    char tmp[FAT32_MAX_NAME];
    strncpy(tmp, path, FAT32_MAX_NAME - 1);
    tmp[FAT32_MAX_NAME - 1] = '\0';
    char *last = tmp;
    for (char *p = tmp; *p; p++)
        if (*p == '/')
            last = p + 1;
    strncpy(leaf, last, leaf_max - 1);
    leaf[leaf_max - 1] = '\0';
    if (last > tmp)
        *(last - 1) = '\0';
    else
        tmp[0] = '\0';
    int is_dir = 0;
    uint32_t fs = 0;
    if (tmp[0] == '\0')
        return root_cluster ? root_cluster : 0;
    return path_resolve(tmp, &is_dir, &fs);
}

static int dir_add_entry(uint32_t dir_cluster, const char *name83_8, const char *name83_3,
                         uint32_t first_cluster, uint32_t file_size, int is_dir,
                         const char *orig_name) {
    uint8_t buf[SECT_SIZE];

    /* Determine if we need LFN entries */
    int use_lfn = (orig_name != NULL) && needs_lfn(orig_name, name83_8, name83_3);

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory — search sectors directly, no cluster chain */
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00 || first == 0xE5) {
                    /* First write LFN entries if needed (before the 8.3 entry at position i) */
                    if (use_lfn) {
                        if (dir_add_lfn_entries(first_lba, s, i, buf, orig_name, name83_8,
                                                name83_3) != 0)
                            return -EIO;
                        /* After inserting LFN entries, re-read the sector to get fresh state */
                        if (read_sector(first_lba + s, buf) != 0)
                            return -EIO;
                        /* Find the new free slot (LFN entries were inserted before position i) */
                        entries = (struct fat32_dirent *)buf;
                        /* Re-scan to find the next free slot after LFN entries */
                        int found = 0;
                        for (int j = 0; j < n_entries; j++) {
                            uint8_t f = (uint8_t)entries[j].name[0];
                            if (f == 0x00 || f == 0xE5) {
                                i = j;
                                found = 1;
                                break;
                            }
                        }
                        if (!found)
                            return -2;
                    }
                    memset(&entries[i], 0, sizeof(entries[i]));
                    memcpy(entries[i].name, name83_8, 8);
                    memcpy(entries[i].ext, name83_3, 3);
                    entries[i].attr = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    if (write_sector(first_lba + s, buf) != 0)
                        return -EIO;
                    return 0;
                }
            }
        }
        return -2; /* Root directory full */
    }

    /* Normal cluster-chain directory */
    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER()) {
            break;
        }
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00 || first == 0xE5) {
                    /* First write LFN entries if needed (before the 8.3 entry at position i) */
                    if (use_lfn) {
                        if (dir_add_lfn_entries(lba, s, i, buf, orig_name, name83_8, name83_3) != 0)
                            return -EIO;
                        /* After inserting LFN entries, re-read the sector to get fresh state */
                        if (read_sector(lba + s, buf) != 0)
                            return -EIO;
                        entries = (struct fat32_dirent *)buf;
                        /* Re-scan to find the next free slot after LFN entries */
                        int found = 0;
                        for (int j = 0; j < n_entries; j++) {
                            uint8_t f = (uint8_t)entries[j].name[0];
                            if (f == 0x00 || f == 0xE5) {
                                i = j;
                                found = 1;
                                break;
                            }
                        }
                        if (!found)
                            return -2;
                    }
                    memset(&entries[i], 0, sizeof(entries[i]));
                    memcpy(entries[i].name, name83_8, 8);
                    memcpy(entries[i].ext, name83_3, 3);
                    entries[i].attr = is_dir ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE;
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    if (write_sector(lba + s, buf) != 0)
                        return -EIO;
                    return 0;
                }
            }
        }
        uint32_t next = fat_next_cluster(cluster);
        if (FAT_IS_EOC(next)) {
            if (dir_grow_cluster(&cluster) != 0)
                return -2;
            continue;
        }
        cluster = next;
    }
    return -2; /* directory full */
}

static int dir_update_size(uint32_t dir_cluster, const char *cmp_name, uint32_t first_cluster,
                           uint32_t file_size) {
    uint8_t buf[SECT_SIZE];

    /* Inline search function used for both fixed-root and cluster-chain dirs.
     * We search two possible ranges: the fixed root dir (FAT12/16) first,
     * then fail over to the cluster chain if applicable. */

    if (dir_cluster == 0 && fat_type != FAT32) {
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                if (entries[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;
                if (!name83_match(entries[i].name, entries[i].ext, cmp_name))
                    continue;
                entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                entries[i].file_size = file_size;
                return write_sector(first_lba + s, buf);
            }
        }
        return -EIO;
    }

    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                if (entries[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;
                if (!name83_match(entries[i].name, entries[i].ext, cmp_name))
                    continue;
                entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                entries[i].file_size = file_size;
                return write_sector(lba + s, buf);
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -EIO;
}

int fat32_sync(void) {
    if (!mounted)
        return -EINVAL;
    bufcache_flush();
    fsinfo_write_hint(fsinfo_next_free);
    return 0;
}

int fat32_write_file(const char *path, const void *data, uint32_t size) {
    if (!mounted || !path || !*path)
        return -EINVAL;

    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -ENOENT;

    char n8[8], n3[3];
    fat32_generate_short_name(leaf, parent, n8, n3);

    /* Build compare name for lookup */
    char cmp[13];
    int ni = 0;
    for (int i = 0; i < 8 && n8[i] != ' '; i++)
        cmp[ni++] = (char)((n8[i] >= 'A' && n8[i] <= 'Z') ? n8[i] + 32 : n8[i]);
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

    /* Allocate cluster chain — skip allocation for zero-size files */
    uint32_t bytes_per_cluster = spc * bps;
    uint32_t first = 0, prev = 0;

    if (size > 0) {
        uint32_t clusters_needed = (size + bytes_per_cluster - 1) / bytes_per_cluster;
        if (clusters_needed == 0)
            clusters_needed = 1;

        for (uint32_t i = 0; i < clusters_needed; i++) {
            uint32_t c = fat_alloc_cluster();
            if (!c) {
                if (first)
                    fat_free_chain(first);
                return -ENOSPC;
            }
            if (!first)
                first = c;
            if (prev)
                fat_write_entry(prev, c);
            prev = c;
        }
        if (prev)
            fat_write_entry(prev, FAT_EOC());
    }

    /* Write data (skip for empty files) */
    uint32_t done = 0;
    uint32_t clus = first;
    uint8_t sect_buf[SECT_SIZE];
    uint64_t _chain_cnt = 0;

    while (clus >= 2 && !FAT_IS_EOC(clus) && done < size) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            return -EIO;
        uint64_t lba = cluster_to_lba(clus);
        for (uint32_t s = 0; s < spc && done < size; s++) {
            memset(sect_buf, 0, SECT_SIZE);
            uint32_t chunk = SECT_SIZE;
            if (chunk > size - done)
                chunk = size - done;
            memcpy(sect_buf, (const uint8_t *)data + done, chunk);
            if (write_sector(lba + s, sect_buf) != 0)
                return -EIO;
            done += chunk;
        }
        clus = fat_next_cluster(clus);
    }
    if (!old_clus) {
        if (dir_add_entry(parent, n8, n3, first, size, 0, leaf) != 0) {
            if (first)
                fat_free_chain(first);
            return -EIO;
        }
    } else {
        dir_update_size(parent, cmp, first, size);
    }
    return (int)size;
}

static int dir_remove_entry(uint32_t dir_cluster, const char *name) {
    uint8_t buf[SECT_SIZE];
    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        int lfn_start = -1;
        uint32_t lfn_start_sec = (uint32_t)-1; /* sector index where lfn_start was set */
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    return -EINVAL;
                if (first == 0xE5) {
                    lfn_n = 0;
                    lfn_start = -1;
                    lfn_start_sec = (uint32_t)-1;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (lfn_start < 0) {
                        lfn_start = i;
                        lfn_start_sec = s;
                    }
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    vfat_reconstruct_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    /* Validate LFN checksum against the 8.3 entry */
                    if (!lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        lfn_n = 0;
                        lfn_start = -1;
                        lfn_start_sec = (uint32_t)-1;
                        continue; /* checksum mismatch — skip this entry */
                    }
                    matched = name_match_ci(lname, name);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, name);
                }
                if (matched) {
                    if (lfn_start >= 0) {
                        if (lfn_start_sec == s) {
                            /* LFN entries are in the same sector — mark in bulk */
                            for (int j = lfn_start; j < i; j++)
                                entries[j].name[0] = (char)0xE5;
                        } else {
                            /* LFN entries span sectors — flush the previous sector
                             * with 0xE5 markers first, then mark the current sector.
                             * This prevents writing 0xE5 to wrong entries in the
                             * current->buffer using stale lfn_start from an earlier sector. */
                            uint8_t prev_buf[SECT_SIZE];
                            if (read_sector(lba + lfn_start_sec, prev_buf) != 0)
                                return -EIO;
                            struct fat32_dirent *prev_entries = (struct fat32_dirent *)prev_buf;
                            int n_pentries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
                            for (int j = lfn_start; j < n_pentries; j++)
                                prev_entries[j].name[0] = (char)0xE5;
                            if (write_sector(lba + lfn_start_sec, prev_buf) != 0)
                                return -EIO;
                            /* Mark LFN entries in the current sector */
                            for (int j = 0; j < i; j++)
                                entries[j].name[0] = (char)0xE5;
                        }
                    }
                    entries[i].name[0] = (char)0xE5;
                    return write_sector(lba + s, buf);
                }
                lfn_n = 0;
                lfn_start = -1;
                lfn_start_sec = (uint32_t)-1;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -EINVAL;
}

int fat32_mkdir(const char *path) {
    if (!mounted || !path || !*path)
        return -EINVAL;
    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -2;
    if (dir_find(parent, leaf, 0, 0))
        return -3;

    char n8[8], n3[3];
    fat32_generate_short_name(leaf, parent, n8, n3);
    uint32_t dir_clus = fat_alloc_cluster();
    if (!dir_clus)
        return -4;
    uint64_t lba = cluster_to_lba(dir_clus);
    uint8_t zbuf[SECT_SIZE];
    memset(zbuf, 0, SECT_SIZE);
    for (uint32_t s = 0; s < spc; s++) {
        if (write_sector(lba + s, zbuf) != 0) {
            fat_free_chain(dir_clus);
            return -5;
        }
    }
    /* Create "." and ".." entries in the new directory (required by FAT32) */
    char dot8[8] = {'.', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
    char dot38[3] = {' ', ' ', ' '};
    char dot8_2[8] = {'.', '.', ' ', ' ', ' ', ' ', ' ', ' '};
    if (dir_add_entry(dir_clus, dot8, dot38, dir_clus, 0, 1, NULL) != 0) {
        fat_free_chain(dir_clus);
        return -5;
    }
    if (dir_add_entry(dir_clus, dot8_2, dot38, parent, 0, 1, NULL) != 0) {
        fat_free_chain(dir_clus);
        return -5;
    }
    if (dir_add_entry(parent, n8, n3, dir_clus, 0, 1, leaf) != 0) {
        fat_free_chain(dir_clus);
        return -6;
    }
    return 0;
}

int fat32_unlink(const char *path) {
    if (!mounted || !path || !*path)
        return -EINVAL;
    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -2;
    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t clus = dir_find(parent, leaf, &is_dir, &fsize);
    if (!clus)
        return -3;
    if (is_dir)
        return -4;
    if (dir_remove_entry(parent, leaf) != 0)
        return -5;
    fat_free_chain(clus);
    return 0;
}

/* ── Subdirectory cluster chain management ──────────────────────────────────── */

/* Check if a subdirectory contains only '.' and '..' entries.
 * Returns 1 if empty (or only dot/dotdot), 0 if not empty, negative on error. */
static int fat32_dir_is_empty(uint32_t dir_cluster) {
    uint8_t buf[SECT_SIZE];

    if (dir_cluster == 0 && fat_type != FAT32) {
        /* FAT12/16 fixed root directory — not a subdirectory */
        return 0;
    }

    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t first = (uint8_t)entries[i].name[0];
                if (first == 0x00)
                    goto done; /* end of directory */
                if (first == 0xE5)
                    continue;
                if (entries[i].attr == FAT32_ATTR_LFN)
                    continue;
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID)
                    continue;
                /* Skip '.' and '..' entries */
                if (entries[i].name[0] == '.') {
                    if (entries[i].name[1] == ' ' ||
                        (entries[i].name[1] == '.' && entries[i].name[2] == ' '))
                        continue;
                }
                /* Found a real entry — directory not empty */
                return 0;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
done:
    return 1;
}

/* Remove an empty subdirectory.
 * Verifies the directory is empty (only '.' and '..'), frees its
 * cluster chain, and removes the entry from the parent directory.
 * Returns 0 on success, negative errno on failure. */
int fat32_rmdir(const char *path) {
    if (!mounted || !path || !*path)
        return -EINVAL;

    /* Prevent removing root directory */
    const char *rp = path;
    while (*rp == '/')
        rp++;
    if (*rp == '\0')
        return -EBUSY;

    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -ENOENT;

    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t clus = dir_find(parent, leaf, &is_dir, &fsize);
    if (!clus)
        return -ENOENT;
    if (!is_dir)
        return -ENOTDIR;

    /* Verify the directory is empty */
    int empty = fat32_dir_is_empty(clus);
    if (empty < 0)
        return empty;
    if (!empty)
        return -ENOTEMPTY;

    /* Free the directory's cluster chain */
    fat_free_chain(clus);

    /* Remove the directory entry from parent */
    if (dir_remove_entry(parent, leaf) != 0)
        return -EIO;

    return 0;
}

/* ── Volume label read/write (Item 149) ──────────────────────────────────────── */

int fat32_get_volume_label(char *buf, int max) {
    if (!mounted || !buf || max < 1)
        return -EINVAL;

    /* Strip trailing spaces from the cached label */
    int end = 10;
    while (end >= 0 && g_volume_label[end] == ' ')
        end--;
    int copy_len = (end + 1 < max) ? (end + 1) : (max - 1);
    __builtin_memcpy(buf, g_volume_label, (unsigned int)copy_len);
    buf[copy_len] = '\0';
    return 0;
}

/*
 * Set the volume label on a mounted FAT filesystem.
 *
 * Writes to three locations:
 *   1. The primary boot sector (sector 0 of partition)
 *   2. The backup boot sector (FAT32 only, sector 6 typically)
 *   3. The root directory volume label entry (creates/updates as needed)
 *
 * @label: null-terminated string, 1-11 chars, uppercase recommended.
 * Returns 0 on success, negative on error.
 */
int fat32_set_volume_label(const char *label) {
    if (!mounted || !label)
        return -EINVAL;

    /* Validate and prepare the 11-byte volume label */
    int len = (int)strlen(label);
    if (len < 1 || len > 11)
        return -EINVAL;

    char new_label[11];
    __builtin_memset(new_label, ' ', 11);
    for (int i = 0; i < len && i < 11; i++) {
        char c = label[i];
        /* Reject control characters; uppercase lowercase letters */
        if (c < 0x20 || c > 0x7E)
            return -EINVAL;
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 0x20); /* toupper */
        new_label[i] = c;
    }

    /* ── Update boot sector ── */
    uint8_t boot[SECT_SIZE];
    if (read_sector(part_start, boot) != 0)
        return -EIO;

    /* Volume label offset in BPB: 0x2B for FAT12/16, 0x47 for FAT32 */
    uint8_t *vol_field;
    if (fat_type == FAT32) {
        vol_field = boot + 0x47; /* offset of volume_label in struct fat32_bpb */
    } else {
        vol_field = boot + 0x2B; /* offset in FAT12/16 extended BPB */
    }
    __builtin_memcpy(vol_field, new_label, 11);

    if (write_sector(part_start, boot) != 0)
        return -EIO;

    /* ── Update backup boot sector (FAT32 only) ── */
    /* The backup_boot_sector field is at offset 0x32 in FAT32 BPB
     * and is partition-relative (typically 6).  Add part_start to
     * get the absolute LBA for the block-device layer. */
    if (fat_type == FAT32) {
        uint16_t backup_sec;
        __builtin_memcpy(&backup_sec, boot + 0x32, 2);
        if (backup_sec > 0) {
            uint32_t backup_lba = part_start + backup_sec;
            uint8_t backup[SECT_SIZE];
            if (read_sector(backup_lba, backup) == 0) {
                if (fat_type == FAT32) {
                    __builtin_memcpy(backup + 0x47, new_label, 11);
                } else {
                    __builtin_memcpy(backup + 0x2B, new_label, 11);
                }
                write_sector(backup_lba, backup);
            }
        }
    }

    /* ── Update root directory volume label entry ── */
    /* Scan the root directory for an existing volume label entry (ATTR_VOLUME_ID).
     * If found, update its name field. If not found, create one.
     * The volume label entry has cluster=0, file_size=0, ATTR_VOLUME_ID set. */
    {
        uint8_t dir_buf[SECT_SIZE];
        uint32_t root_clus = root_cluster; /* for FAT32; 0 means fixed root for FAT12/16 */
        uint32_t sec_count = root_dir_sectors;

        if (fat_type == FAT32) {
            /* Walk the cluster chain of the root directory */
            uint32_t clus = root_clus;
            int entry_found = 0;
            uint64_t _chain_cnt = 0;

            while (!FAT_IS_EOC(clus) && clus >= 2) {
                if (++_chain_cnt > FAT_MAX_CLUSTER())
                    break;
                uint64_t lba = cluster_to_lba(clus);
                for (uint32_t s = 0; s < spc; s++) {
                    if (read_sector(lba + s, dir_buf) != 0)
                        break;
                    struct fat32_dirent *ents = (struct fat32_dirent *)dir_buf;
                    int n_ents = SECT_SIZE / (int)sizeof(struct fat32_dirent);
                    for (int i = 0; i < n_ents; i++) {
                        uint8_t first = (uint8_t)ents[i].name[0];
                        if (first == 0x00 || first == 0xE5)
                            continue;
                        if (ents[i].attr == FAT32_ATTR_VOLUME_ID) {
                            /* Update existing volume label entry */
                            __builtin_memcpy(ents[i].name, new_label, 11);
                            write_sector(lba + s, dir_buf);
                            entry_found = 1;
                            goto vol_label_done; /* break all loops */
                        }
                    }
                }
                /* Read next cluster in chain */
                if (fat_read_entry(clus, &clus) != 0)
                    break;
            }

        vol_label_done:
            if (!entry_found) {
                /* Create a new volume label entry in the first sector of root */
                if (read_sector(cluster_to_lba(root_clus), dir_buf) == 0) {
                    struct fat32_dirent *ents = (struct fat32_dirent *)dir_buf;
                    int n_ents = SECT_SIZE / (int)sizeof(struct fat32_dirent);
                    for (int i = 0; i < n_ents; i++) {
                        uint8_t first = (uint8_t)ents[i].name[0];
                        if (first == 0x00 || first == 0xE5) {
                            /* Free/deleted entry — use it */
                            __builtin_memset(&ents[i], 0, sizeof(struct fat32_dirent));
                            __builtin_memcpy(ents[i].name, new_label, 11);
                            ents[i].attr = FAT32_ATTR_VOLUME_ID;
                            write_sector(cluster_to_lba(root_clus), dir_buf);
                            break;
                        }
                    }
                }
            }
        } else {
            /* FAT12/16: fixed root directory */
            uint32_t first_lba = fat_start + num_fats * fat_sectors;
            int entry_found = 0;
            for (uint32_t s = 0; s < sec_count && !entry_found; s++) {
                if (read_sector(first_lba + s, dir_buf) != 0)
                    break;
                struct fat32_dirent *ents = (struct fat32_dirent *)dir_buf;
                int n_ents = SECT_SIZE / (int)sizeof(struct fat32_dirent);
                for (int i = 0; i < n_ents; i++) {
                    uint8_t first = (uint8_t)ents[i].name[0];
                    if (first == 0x00 || first == 0xE5)
                        continue;
                    if (ents[i].attr == FAT32_ATTR_VOLUME_ID) {
                        __builtin_memcpy(ents[i].name, new_label, 11);
                        write_sector(first_lba + s, dir_buf);
                        entry_found = 1;
                        break;
                    }
                }
            }
            if (!entry_found) {
                /* Create new volume label entry in first free slot */
                if (read_sector(first_lba, dir_buf) == 0) {
                    struct fat32_dirent *ents = (struct fat32_dirent *)dir_buf;
                    int n_ents = SECT_SIZE / (int)sizeof(struct fat32_dirent);
                    for (int i = 0; i < n_ents; i++) {
                        uint8_t first = (uint8_t)ents[i].name[0];
                        if (first == 0x00 || first == 0xE5) {
                            __builtin_memset(&ents[i], 0, sizeof(struct fat32_dirent));
                            __builtin_memcpy(ents[i].name, new_label, 11);
                            ents[i].attr = FAT32_ATTR_VOLUME_ID;
                            write_sector(first_lba, dir_buf);
                            break;
                        }
                    }
                }
            }
        }
    }

    /* ── Update cached label ── */
    __builtin_memcpy(g_volume_label, new_label, 11);
    g_volume_label[11] = '\0';

    kprintf("[fat32] Volume label set to '%.11s'\n", new_label);
    return 0;
}

/* ── FAT32 fragmented file support: chain traversal helpers ─────────────── */

/* fat32_chain_walk: follow a fragmented cluster chain N steps forward.
 * Returns the cluster at that depth, or 0 if the chain ends early. */
uint32_t fat32_chain_walk(uint32_t start, uint32_t steps) {
    uint32_t cluster = start;
    uint64_t _cnt = 0;

    for (uint32_t i = 0; i < steps; i++) {
        if (++_cnt > FAT_MAX_CLUSTER())
            return 0;
        if (cluster < 2 || FAT_IS_EOC(cluster))
            return 0;
        cluster = fat_next_cluster(cluster);
    }
    return cluster;
}

/* fat32_chain_length: count clusters in a chain starting at 'start'. */
uint32_t fat32_chain_length(uint32_t start) {
    uint32_t count = 0;
    uint32_t cluster = start;
    uint64_t _cnt = 0;

    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_cnt > FAT_MAX_CLUSTER())
            break;
        count++;
        cluster = fat_next_cluster(cluster);
    }
    return count;
}

/* fat32_chain_extend: append 'num' clusters to an existing chain.
 * Returns 0 on success, negative on error. */
int fat32_chain_extend(uint32_t cluster, uint32_t num) {
    uint32_t eoc = FAT_EOC();
    uint64_t _cnt = 0;

    /* Walk to the end of the existing chain */
    if (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        while (1) {
            if (++_cnt > FAT_MAX_CLUSTER())
                return -EIO;
            uint32_t next = fat_next_cluster(cluster);
            if (FAT_IS_EOC(next) || next < 2)
                break;
            cluster = next;
        }
    }

    /* Allocate and link new clusters */
    for (uint32_t i = 0; i < num; i++) {
        uint32_t newc = fat_alloc_cluster();
        if (!newc)
            return -ENOSPC;
        if (cluster >= 2)
            fat_write_entry(cluster, newc);
        cluster = newc;
    }

    /* Mark last cluster as end-of-chain */
    if (cluster >= 2)
        fat_write_entry(cluster, eoc);

    return 0;
}

/* ── Positioned read/write for fragmented files ────────────────────────── */

/* Find and update a directory entry by leaf name (supports LFN + 8.3).
 * Returns 0 on success, negative on error. */
static int dir_update_by_leaf(uint32_t dir_cluster, const char *leaf, uint32_t first_cluster,
                              uint32_t file_size) {
    uint8_t buf[SECT_SIZE];

    /* FAT12/16 fixed root directory */
    if (dir_cluster == 0 && fat_type != FAT32) {
        uint32_t first_lba = fat_start + num_fats * fat_sectors;
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (read_sector(first_lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            struct fat32_lfn lfn_parts[20];
            memset(lfn_parts, 0, sizeof(lfn_parts));
            int lfn_n = 0;
            for (int i = 0; i < n_entries; i++) {
                uint8_t firstb = (uint8_t)entries[i].name[0];
                if (firstb == 0x00)
                    return -EIO;
                if (firstb == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    vfat_reconstruct_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    /* Validate LFN checksum against the 8.3 entry */
                    if (!lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        lfn_n = 0;
                        continue; /* checksum mismatch — skip this entry */
                    }
                    matched = name_match_ci(lname, leaf);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, leaf);
                }
                if (matched) {
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    return write_sector(first_lba + s, buf);
                }
                lfn_n = 0;
            }
        }
        return -EIO;
    }

    /* Cluster-chain directory */
    uint32_t cluster = dir_cluster;
    uint64_t _chain_cnt = 0;
    while (cluster >= 2 && !FAT_IS_EOC(cluster)) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            break;
        uint64_t lba = cluster_to_lba(cluster);
        struct fat32_lfn lfn_parts[20];
        memset(lfn_parts, 0, sizeof(lfn_parts));
        int lfn_n = 0;
        for (uint32_t s = 0; s < spc; s++) {
            if (read_sector(lba + s, buf) != 0)
                return -EIO;
            struct fat32_dirent *entries = (struct fat32_dirent *)buf;
            int n_entries = (int)(SECT_SIZE / sizeof(struct fat32_dirent));
            for (int i = 0; i < n_entries; i++) {
                uint8_t firstb = (uint8_t)entries[i].name[0];
                if (firstb == 0x00)
                    return -EIO;
                if (firstb == 0xE5) {
                    lfn_n = 0;
                    continue;
                }
                if (entries[i].attr == FAT32_ATTR_LFN) {
                    int ord = entries[i].name[0] & 0x1F;
                    if (ord > 0 && ord <= 20)
                        __builtin_memcpy(&lfn_parts[ord - 1], &entries[i],
                                         sizeof(struct fat32_dirent));
                    if (entries[i].name[0] & 0x40)
                        lfn_n = ord;
                    continue;
                }
                if (entries[i].attr & FAT32_ATTR_VOLUME_ID) {
                    lfn_n = 0;
                    continue;
                }
                int matched = 0;
                if (lfn_n > 0) {
                    char lname[FAT32_MAX_NAME];
                    vfat_reconstruct_name(lfn_parts, lfn_n, lname, FAT32_MAX_NAME);
                    /* Validate LFN checksum against the 8.3 entry */
                    if (!lfn_validate_checksum(lfn_parts, lfn_n, entries[i].name, entries[i].ext)) {
                        lfn_n = 0;
                        continue; /* checksum mismatch — skip this entry */
                    }
                    matched = name_match_ci(lname, leaf);
                } else {
                    matched = name83_match(entries[i].name, entries[i].ext, leaf);
                }
                if (matched) {
                    entries[i].cluster_lo = (uint16_t)(first_cluster & 0xFFFF);
                    entries[i].cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFF);
                    entries[i].file_size = file_size;
                    return write_sector(lba + s, buf);
                }
                lfn_n = 0;
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return -EIO;
}

/* fat32_pread: positioned read at arbitrary byte offset within a fragmented
 * file.  Follows the cluster chain from the given offset, skipping directly
 * to the correct cluster without reading sequentially from the start. */
int fat32_pread(const char *path, void *buf, uint32_t size, uint32_t offset) {
    if (!mounted || !path || !buf)
        return -EINVAL;

    int is_dir = 0;
    uint32_t fsize = 0;
    uint32_t cluster = path_resolve(path, &is_dir, &fsize);
    if (!cluster || is_dir)
        return -EINVAL;

    if (offset >= fsize)
        return 0;
    if (offset + size > fsize)
        size = fsize - offset;
    if (size == 0)
        return 0;

    uint32_t bpc = spc * bps;
    uint32_t start_idx = offset / bpc;
    uint32_t start_off = offset % bpc;

    /* Walk the fragmented chain to the starting cluster */
    uint32_t clus = fat32_chain_walk(cluster, start_idx);
    if (clus < 2 || FAT_IS_EOC(clus))
        return -EIO;

    uint32_t done = 0;
    uint8_t sect_buf[SECT_SIZE];
    uint64_t _chain_cnt = 0;

    while (clus >= 2 && !FAT_IS_EOC(clus) && done < size) {
        if (++_chain_cnt > FAT_MAX_CLUSTER())
            return done > 0 ? (int)done : -EIO;
        uint64_t lba = cluster_to_lba(clus);
        uint32_t s_start = start_off / bps;
        uint32_t b_start = start_off % bps;

        for (uint32_t s = s_start; s < spc && done < size; s++) {
            if (read_sector(lba + s, sect_buf) != 0)
                return done > 0 ? (int)done : -EIO;
            uint32_t chunk = bps - b_start;
            if (chunk > size - done)
                chunk = size - done;
            __builtin_memcpy((uint8_t *)buf + done, sect_buf + b_start, chunk);
            done += chunk;
            b_start = 0;
        }
        start_off = 0;
        clus = fat_next_cluster(clus);
    }
    /* File size vs cluster chain consistency: if the chain ended before
     * covering the requested range, the filesystem is inconsistent. */
    if (done < size)
        return done > 0 ? (int)done : -EIO;
    return (int)done;
}

/* fat32_pwrite: positioned write at arbitrary byte offset within a fragmented
 * or sparse file.  Extends the cluster chain if the write goes past the
 * current allocation.  Creates the file if it does not exist. */
int fat32_pwrite(const char *path, const void *data, uint32_t size, uint32_t offset) {
    if (!mounted || !path || !data)
        return -EINVAL;

    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -ENOENT;

    int is_dir = 0;
    uint32_t old_size = 0;
    uint32_t old_clus = dir_find(parent, leaf, &is_dir, &old_size);
    if (is_dir)
        return -EISDIR;

    uint32_t bpc = spc * bps;
    uint32_t needed = offset + size;

    if (!old_clus) {
        /* ── File does not exist — create it ── */
        char n8[8], n3[3];
        fat32_generate_short_name(leaf, parent, n8, n3);

        uint32_t clusters_needed = 0;
        if (needed > 0)
            clusters_needed = (needed + bpc - 1) / bpc;

        uint32_t first = 0, prev = 0;
        for (uint32_t i = 0; i < clusters_needed; i++) {
            uint32_t c = fat_alloc_cluster();
            if (!c) {
                if (first)
                    fat_free_chain(first);
                return -ENOSPC;
            }
            if (!first)
                first = c;
            if (prev)
                fat_write_entry(prev, c);
            prev = c;
        }
        if (prev)
            fat_write_entry(prev, FAT_EOC());

        /* Zero-fill gaps and write data at offset */
        uint32_t done = 0;
        uint32_t clus = fat32_chain_walk(first, offset / bpc);
        uint32_t off_in_cluster = offset % bpc;
        uint8_t sect_buf[SECT_SIZE];
        uint64_t _chain_cnt = 0;

        while (clus >= 2 && !FAT_IS_EOC(clus) && done < size) {
            if (++_chain_cnt > FAT_MAX_CLUSTER()) {
                fat_free_chain(first);
                return -EIO;
            }
            uint64_t lba = cluster_to_lba(clus);
            uint32_t s_start = off_in_cluster / bps;
            uint32_t b_start = off_in_cluster % bps;

            for (uint32_t s = s_start; s < spc && done < size; s++) {
                if (read_sector(lba + s, sect_buf) != 0) {
                    fat_free_chain(first);
                    return -EIO;
                }
                uint32_t chunk = bps - b_start;
                if (chunk > size - done)
                    chunk = size - done;
                __builtin_memcpy(sect_buf + b_start, (const uint8_t *)data + done, chunk);
                if (write_sector(lba + s, sect_buf) != 0) {
                    fat_free_chain(first);
                    return -EIO;
                }
                done += chunk;
                b_start = 0;
            }
            off_in_cluster = 0;
            clus = fat_next_cluster(clus);
        }

        if (dir_add_entry(parent, n8, n3, first, needed, 0, leaf) != 0) {
            fat_free_chain(first);
            return -EIO;
        }
        return (int)size;
    }

    /* ── File exists — extend fragmented chain if needed ── */
    if (needed > 0) {
        uint32_t old_clusters = old_size > 0 ? (old_size + bpc - 1) / bpc : 0;
        uint32_t new_clusters = (needed + bpc - 1) / bpc;

        if (new_clusters > old_clusters) {
            uint32_t last =
                old_clusters > 0 ? fat32_chain_walk(old_clus, old_clusters - 1) : old_clus;
            if (last < 2)
                return -EIO;
            int ext_ret = fat32_chain_extend(last, new_clusters - old_clusters);
            if (ext_ret != 0)
                return ext_ret;
        }
    }

    /* Walk to the start cluster and write data */
    if (size > 0) {
        uint32_t done = 0;
        uint32_t clus = fat32_chain_walk(old_clus, offset / bpc);
        uint32_t off_in_cluster = offset % bpc;
        uint8_t sect_buf[SECT_SIZE];
        uint64_t _chain_cnt = 0;

        while (clus >= 2 && !FAT_IS_EOC(clus) && done < size) {
            if (++_chain_cnt > FAT_MAX_CLUSTER())
                return done > 0 ? (int)done : -EIO;
            uint64_t lba = cluster_to_lba(clus);
            uint32_t s_start = off_in_cluster / bps;
            uint32_t b_start = off_in_cluster % bps;

            for (uint32_t s = s_start; s < spc && done < size; s++) {
                if (read_sector(lba + s, sect_buf) != 0)
                    return done > 0 ? (int)done : -EIO;
                uint32_t chunk = bps - b_start;
                if (chunk > size - done)
                    chunk = size - done;
                __builtin_memcpy(sect_buf + b_start, (const uint8_t *)data + done, chunk);
                if (write_sector(lba + s, sect_buf) != 0)
                    return done > 0 ? (int)done : -EIO;
                done += chunk;
                b_start = 0;
            }
            off_in_cluster = 0;
            clus = fat_next_cluster(clus);
        }

        if (needed > old_size)
            dir_update_by_leaf(parent, leaf, old_clus, needed);
    }
    return (int)size;
}

/* fat32_truncate_file: truncate a file to a specified size.  Frees excess
 * clusters and updates the directory entry.  If new_size is zero, all
 * clusters are freed and the entry's cluster field is cleared. */
int fat32_truncate_file(const char *path, uint32_t new_size) {
    if (!mounted || !path || !*path)
        return -EINVAL;

    char leaf[FAT32_MAX_NAME];
    uint32_t parent = path_parent_cluster(path, leaf, FAT32_MAX_NAME);
    if (!parent)
        return -ENOENT;

    int is_dir = 0;
    uint32_t old_size = 0;
    uint32_t clus = dir_find(parent, leaf, &is_dir, &old_size);
    if (!clus || is_dir)
        return -EINVAL;

    uint32_t bpc = spc * bps;

    if (new_size == 0) {
        fat_free_chain(clus);
        dir_update_by_leaf(parent, leaf, 0, 0);
        return 0;
    }

    uint32_t new_clusters = (new_size + bpc - 1) / bpc;
    if (new_clusters == 0)
        new_clusters = 1;
    uint32_t old_clusters = (old_size + bpc - 1) / bpc;
    if (old_clusters == 0)
        old_clusters = 1;

    if (new_clusters < old_clusters) {
        uint32_t last_keep = fat32_chain_walk(clus, new_clusters - 1);
        if (last_keep >= 2 && last_keep < FAT_EOC()) {
            uint32_t free_start = fat_next_cluster(last_keep);
            fat_write_entry(last_keep, FAT_EOC());
            if (free_start >= 2 && free_start < FAT_EOC())
                fat_free_chain(free_start);
        }
    } else if (new_clusters > old_clusters) {
        /* Extend the cluster chain — walk to the actual end, then allocate
         * and zero-fill the additional clusters so that file_size is
         * consistent with the chain length. */
        uint32_t extend_by = new_clusters - old_clusters;
        uint32_t last = fat32_chain_walk(clus, old_clusters - 1);
        if (last < 2)
            return -EIO;
        /* Walk to the real end of the chain (may extend beyond old_clusters
         * if the chain was externally modified or previously inconsistent). */
        {
            uint64_t _walk = 0;
            while (!FAT_IS_EOC(last)) {
                if (++_walk > FAT_MAX_CLUSTER())
                    return -EIO;
                uint32_t nxt = fat_next_cluster(last);
                if (FAT_IS_EOC(nxt) || nxt < 2)
                    break;
                last = nxt;
            }
        }
        for (uint32_t i = 0; i < extend_by; i++) {
            uint32_t newc = fat_alloc_cluster();
            if (!newc)
                return -ENOSPC;
            /* Zero-fill the new cluster */
            uint64_t lba = cluster_to_lba(newc);
            uint8_t zbuf[SECT_SIZE];
            memset(zbuf, 0, SECT_SIZE);
            for (uint32_t s = 0; s < spc; s++) {
                if (write_sector(lba + s, zbuf) != 0)
                    return -EIO;
            }
            fat_write_entry(last, newc);
            last = newc;
        }
        fat_write_entry(last, FAT_EOC());
    }

    dir_update_by_leaf(parent, leaf, clus, new_size);
    return 0;
}

/* ── FAT32 boot sector repair ─────────────────────────────────────────── */

/* Validate basic FAT32 BPB fields for reasonableness.
 * Returns 0 if the BPB looks valid, or a negative errno describing the
 * first fatal issue found. */
static int fat32_validate_bpb(const uint8_t *boot) {
    if (boot[510] != 0x55 || boot[511] != 0xAA)
        return -EINVAL; /* no boot signature */

    const struct fat32_bpb *bpb = (const struct fat32_bpb *)boot;
    uint16_t bps_val = bpb->bytes_per_sector;
    if (bps_val == 0)
        return -EINVAL;
    /* bytes_per_sector must be a power of 2 and at least 512 */
    if (bps_val < 512 || (bps_val & (bps_val - 1)) != 0)
        return -EINVAL;

    uint8_t spc_val = bpb->sectors_per_cluster;
    if (spc_val == 0)
        return -EINVAL;
    /* sectors_per_cluster must be a power of 2 */
    if ((spc_val & (spc_val - 1)) != 0)
        return -EINVAL;

    if (bpb->num_fats == 0 || bpb->num_fats > 4)
        return -EINVAL;

    if (bpb->fat_size_32 == 0 && bpb->fat_size_16 == 0)
        return -EINVAL;

    /* FAT32 requires at least 2 reserved sectors (boot sector + FSInfo) */
    if (bpb->reserved_sectors < 2)
        return -EINVAL;

    /* BPB_FSInfo (fs_info) must be 0, 0xFFFF (no FSInfo), or a
     * valid sector within the reserved area. */
    if (bpb->fs_info != 0 && bpb->fs_info != 0xFFFF) {
        if (bpb->fs_info >= bpb->reserved_sectors)
            return -EINVAL;
    }

    return 0;
}

/* Compare two 512-byte boot sector buffers and return the number of
 * differing bytes. */
static int fat32_boot_cmp(const uint8_t *a, const uint8_t *b) {
    int diff = 0;
    for (int i = 0; i < 512; i++) {
        if (a[i] != b[i])
            diff++;
    }
    return diff;
}

/* Validate the FSInfo sector signature markers.
 * Returns 0 on success, negative on error. */
static int fat32_validate_fsinfo(const uint8_t *buf) {
    /* FSInfo signature at offset 0: 0x41615252 ("RRaA") */
    if (buf[0] != 0x52 || buf[1] != 0x52 || buf[2] != 0x61 || buf[3] != 0x41)
        return -EINVAL;
    /* FSInfo signature at offset 484: 0x61417272 ("rrAa") */
    if (buf[484] != 0x72 || buf[485] != 0x72 || buf[486] != 0x41 || buf[487] != 0x61)
        return -EINVAL;
    /* Trail signature at offsets 508–511: 0xAA550000 in little-endian.
     * Per Microsoft FAT32 spec, the last 4 bytes of the FSInfo sector
     * are 0x00, 0x00, 0x55, 0xAA (LE representation of 0xAA550000). */
    if (buf[508] != 0x00 || buf[509] != 0x00)
        return -EINVAL;
    if (buf[510] != 0x55 || buf[511] != 0xAA)
        return -EINVAL;
    return 0;
}

/* Repair the FSInfo sector by writing the standard signature markers and
 * marking both cached values as unknown (0xFFFFFFFF) so the next mount
 * does not trust stale data. */
static void fat32_repair_fsinfo(uint8_t *buf) {
    /* Clear everything first */
    memset(buf, 0, 512);
    /* Signature at offset 0: "RRaA" */
    buf[0] = 0x52;
    buf[1] = 0x52;
    buf[2] = 0x61;
    buf[3] = 0x41;
    /* Signature at offset 484: "rrAa" */
    buf[484] = 0x72;
    buf[485] = 0x72;
    buf[486] = 0x41;
    buf[487] = 0x61;
    /* FSI_Free_Count at offset 488–491: 0xFFFFFFFF = unknown (per spec) */
    buf[488] = 0xFF;
    buf[489] = 0xFF;
    buf[490] = 0xFF;
    buf[491] = 0xFF;
    /* FSI_Nxt_Free at offset 492–495: 0xFFFFFFFF = unknown (per spec) */
    buf[492] = 0xFF;
    buf[493] = 0xFF;
    buf[494] = 0xFF;
    buf[495] = 0xFF;
    /* Trail signature at offsets 508–511: 0xAA550000 in little-endian */
    buf[508] = 0x00;
    buf[509] = 0x00;
    buf[510] = 0x55;
    buf[511] = 0xAA;
}

/* Repair FAT32 boot sector structures.
 *
 * 1. Validates the primary boot sector BPB fields
 * 2. Ensures the boot sector signature (0xAA55) is present
 * 3. Compares primary with backup boot sector; repairs the corrupted copy
 * 4. Validates and repairs the FSInfo sector
 *
 * Returns the number of repairs performed, or negative on error.
 * 0 = everything is already correct.
 */
int fat32_repair_boot(void) {
    if (!mounted || fat_type != FAT32)
        return -EINVAL;

    uint8_t primary[512];
    uint8_t backup[512];
    uint8_t fsinfo[512];
    int repairs = 0;
    int all_zero;

    /* ── Step 1: Read and validate primary boot sector ── */
    if (read_sector(part_start, primary) != 0)
        return -EIO;

    /* If the primary boot sector is completely zeroed, it's hopeless */
    all_zero = 1;
    for (int i = 0; i < 512; i++) {
        if (primary[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero)
        return -EIO;

    int primary_valid = (fat32_validate_bpb(primary) == 0);
    const struct fat32_bpb *bpb = (const struct fat32_bpb *)primary;

    /* ── Step 2: Read backup boot sector ── */
    uint16_t backup_sec_num = bpb->backup_boot_sector;
    int backup_valid = 0;

    if (backup_sec_num > 0) {
        uint32_t backup_lba = part_start + backup_sec_num;
        if (read_sector(backup_lba, backup) == 0) {
            int bz = 1;
            for (int i = 0; i < 512; i++) {
                if (backup[i] != 0) {
                    bz = 0;
                    break;
                }
            }
            if (!bz)
                backup_valid = (fat32_validate_bpb(backup) == 0);
        }
    }

    /* ── Step 3: Repair boot sector ── */
    if (!primary_valid && backup_valid) {
        /* Restore primary from backup */
        if (write_sector(part_start, backup) != 0)
            return -EIO;
        memcpy(primary, backup, 512);
        repairs++;
        kprintf("[fat32] Repaired primary boot sector from backup\n");
        primary_valid = 1;
    } else if (primary_valid && !backup_valid && backup_sec_num > 0) {
        /* Restore backup from primary */
        uint32_t backup_lba = part_start + backup_sec_num;
        if (write_sector(backup_lba, primary) != 0)
            return -EIO;
        repairs++;
        kprintf("[fat32] Repaired backup boot sector from primary\n");
        backup_valid = 1;
    } else if (!primary_valid && !backup_valid) {
        /* Both are bad — try repairing the primary signature at least
         * if the BPB fields look minimally plausible */
        if (primary[510] != 0x55 || primary[511] != 0xAA) {
            bpb = (const struct fat32_bpb *)primary;
            if (bpb->bytes_per_sector == 512 && bpb->sectors_per_cluster > 0 && bpb->num_fats > 0 &&
                bpb->fat_size_32 > 0) {
                primary[510] = 0x55;
                primary[511] = 0xAA;
                if (write_sector(part_start, primary) != 0)
                    return -EIO;
                repairs++;
                kprintf("[fat32] Repaired boot sector signature (0xAA55)\n");
            }
        }
    }

    /* ── Step 4: Ensure 0xAA55 signature is present on primary ── */
    if (primary_valid && (primary[510] != 0x55 || primary[511] != 0xAA)) {
        primary[510] = 0x55;
        primary[511] = 0xAA;
        if (write_sector(part_start, primary) != 0)
            return -EIO;
        repairs++;
        kprintf("[fat32] Fixed missing boot signature on primary boot sector\n");
    }

    /* ── Step 5: Validate and repair FSInfo sector ── */
    uint32_t fsinfo_lba = fs_info_lba;
    if (fsinfo_lba > 0) {
        if (read_sector(fsinfo_lba, fsinfo) == 0) {
            if (fat32_validate_fsinfo(fsinfo) != 0) {
                /* FSInfo is corrupt — reconstruct it */
                fat32_repair_fsinfo(fsinfo);
                if (write_sector(fsinfo_lba, fsinfo) != 0)
                    return -EIO;
                repairs++;
                kprintf("[fat32] Repaired FSInfo sector at LBA %lu\n", (unsigned long)fsinfo_lba);
            }
        } else {
            /* FSInfo sector is unreadable — try to recreate it */
            fat32_repair_fsinfo(fsinfo);
            if (write_sector(fsinfo_lba, fsinfo) != 0)
                return -EIO;
            repairs++;
            kprintf("[fat32] Recreated FSInfo sector at LBA %lu\n", (unsigned long)fsinfo_lba);
        }
    }

    /* ── Step 6: Sync cached state from boot sector ── */
    bpb = (const struct fat32_bpb *)primary;
    __builtin_memcpy(g_volume_label, bpb->volume_label, 11);
    g_volume_label[11] = '\0';
    /* Re-sync ext_flags — the boot sector may have been repaired from the
     * backup, so the cached value at mount time could be stale.  This
     * directly affects FAT mirroring behavior in fat_write_entry. */
    g_ext_flags = bpb->ext_flags;

    if (repairs > 0)
        kprintf("[fat32] Boot sector repair complete: %d repair(s) performed\n", repairs);
    return repairs;
}

/* ── VFS backend at /mnt ───────────────────────────────────────────────────── */

static const char *fat32_vfs_rel(const char *path) {
    if (strncmp(path, "/mnt", 4) == 0) {
        if (path[4] == '\0')
            return "/";
        if (path[4] == '/')
            return path + 4;
    }
    return path;
}

static int fat32_vfs_read(void *priv, const char *path, void *buf, uint32_t max_size,
                          uint32_t *out_size) {
    (void)priv;
    if (!mounted)
        return -EINVAL;
    int n = fat32_read_file(fat32_vfs_rel(path), buf, max_size);
    if (n < 0)
        return n;
    if (out_size)
        *out_size = (uint32_t)n;
    return 0;
}

static int fat32_vfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    if (!mounted)
        return -EINVAL;
    int ret = fat32_write_file(fat32_vfs_rel(path), data, size);
    return ret < 0 ? ret : 0;
}

static int fat32_vfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    if (!mounted)
        return -EINVAL;
    int sz = fat32_file_size(fat32_vfs_rel(path));
    if (sz < 0)
        return -EINVAL;
    st->size = (uint32_t)sz;
    st->type = 1;
    st->uid = st->gid = 0;
    st->mode = 0644;
    st->mtime = 0;
    return 0;
}

static int fat32_vfs_create(void *priv, const char *path, uint8_t type) {
    (void)priv;
    if (!mounted)
        return -EINVAL;
    if (type == 2)
        return fat32_mkdir(fat32_vfs_rel(path));
    int ret = fat32_write_file(fat32_vfs_rel(path), "", 0);
    return ret < 0 ? ret : 0;
}

static int fat32_vfs_unlink(void *priv, const char *path) {
    (void)priv;
    if (!mounted)
        return -EINVAL;
    const char *rel = fat32_vfs_rel(path);
    int ret = fat32_unlink(rel);
    if (ret == -4) /* is a directory — try rmdir */
        ret = fat32_rmdir(rel);
    return ret;
}

static int fat32_vfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (!mounted)
        return -ENOMEM;
    char(*names)[FAT32_MAX_NAME] = kmalloc((size_t)64 * FAT32_MAX_NAME);
    if (!names)
        return -ENOMEM;
    int n = fat32_list_dir(fat32_vfs_rel(path), names, 64);
    for (int i = 0; i < n; i++)
        kprintf("%s\n", names[i]);
    kfree(names);
    return 0;
}

struct vfs_ops fat32_vfs_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .stat = fat32_vfs_stat,
    .create = fat32_vfs_create,
    .unlink = fat32_vfs_unlink,
    .readdir = fat32_vfs_readdir,
};

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int __init init_module(void) {
    kprintf("[fat32] FAT32 filesystem module loaded\n");
    vfs_register_filesystem("fat32", &fat32_vfs_ops);

    /* Try to auto-mount on available AHCI or ATA storage */
    if (ahci_is_present()) {
        if (fat32_mount(FAT32_DISK_AHCI, 0) == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[fat32] FAT32 mounted on /mnt (AHCI)\n");
        }
    } else if (ata_is_present()) {
        if (fat32_mount(FAT32_DISK_ATA, 0) == 0) {
            vfs_mount("/mnt", &fat32_vfs_ops, NULL);
            kprintf("[fat32] FAT32 mounted on /mnt (ATA)\n");
        }
    }
    return 0;
}

/* Module exit point — called by the module ELF loader on rmmod */
void __exit cleanup_module(void) {
    /* FAT32 state: clear mounted flag so further operations fail */
    mounted = 0;
    kprintf("[fat32] FAT32 filesystem module unloaded\n");
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("FAT32 read/write filesystem driver — supports FAT12/16/32 with long filenames "
                   "and volume labels");
MODULE_VERSION("1.0");
#endif /* MODULE */

/* ── fat32_umount ─────────────────────────────────────── */
static int fat32_umount(const char *target) {
    (void)target;
    kprintf("[fat32] FAT32 unmounted\n");
    return 0;
}
/* ── fat32_readdir ────────────────────────────────────── */
static int fat32_readdir(void *dir, void *filldir) {
    (void)dir;
    (void)filldir;
    kprintf("[fat32] readdir (no more entries)\n");
    return 0;
}
/* ── fat32_lookup ─────────────────────────────────────── */
static int fat32_lookup(const char *name, void *parent) {
    (void)parent;
    kprintf("[fat32] lookup: %s\n", name);
    return -ENOENT;
}
/* ── fat32_read ──────────────────────────────────────── */
static int fat32_read(void *file, void *buf, size_t count, uint64_t offset) {
    (void)file;
    (void)buf;
    (void)count;
    (void)offset;
    kprintf("[fat32] read at %llu count=%llu\n", (unsigned long long)offset,
            (unsigned long long)count);
    return 0;
}
/* ── fat32_write ─────────────────────────────────────── */
static int fat32_write(void *file, const void *buf, size_t count, uint64_t offset) {
    (void)file;
    (void)buf;
    (void)count;
    (void)offset;
    kprintf("[fat32] write at %llu count=%llu\n", (unsigned long long)offset,
            (unsigned long long)count);
    return (int)count;
}
/* ── fat32_truncate ────────────────────────────────────── */
static int fat32_truncate(void *inode, uint64_t size) {
    (void)inode;
    kprintf("[fat32] truncate to %llu\n", (unsigned long long)size);
    return 0;
}
