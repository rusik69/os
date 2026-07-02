#ifndef EXFAT_H
#define EXFAT_H

#include "types.h"
#include "vfs.h"

#define EXFAT_MAGIC         0x2020202054414645ULL  /* "EXFAT   " */
#define EXFAT_SECTOR_SIZE   512
#define EXFAT_MAX_NAME      255
#define EXFAT_UPCASE_SECTOR 2
#define EXFAT_FAT_SECTOR    2      /* typically sector 2 is FAT start placeholder */

/* Entry types */
#define EXFAT_ENTRY_EOD            0x00
#define EXFAT_ENTRY_UNUSED         0x01
#define EXFAT_ENTRY_BITMAP         0x81
#define EXFAT_ENTRY_UPCASE         0x82
#define EXFAT_ENTRY_LABEL          0x83
#define EXFAT_ENTRY_FILE           0x85
#define EXFAT_ENTRY_STREAM_EXT     0xC0
#define EXFAT_ENTRY_FILE_NAME      0xC1
#define EXFAT_ENTRY_VENDOR_EXT     0xC2
#define EXFAT_ENTRY_VENDOR_ALLOC   0xC3

/* File attributes */
#define EXFAT_ATTR_READONLY  0x0001

/* Stream extension general_secondary_flags */
#define EXFAT_FLAG_NO_FAT_CHAIN  0x01  /* clusters are contiguous, no FAT traversal needed */
#define EXFAT_FLAG_FAT_MIRROR    0x02  /* FAT mirroring enabled */
#define EXFAT_ATTR_HIDDEN    0x0002
#define EXFAT_ATTR_SYSTEM    0x0004
#define EXFAT_ATTR_DIRECTORY 0x0010
#define EXFAT_ATTR_ARCHIVE   0x0020

/* Entry type masks */
#define EXFAT_TYPE_MASK   0x80
#define EXFAT_SECONDARY_MASK  0x40
#define EXFAT_IMPORTANT_MASK  0x20

/* Cluster values */
#define EXFAT_CLUSTER_FREE      0
#define EXFAT_CLUSTER_BAD       0xFFFFFFF7
#define EXFAT_CLUSTER_END       0xFFFFFFFF

#pragma pack(push, 1)

/* Boot sector */
struct exfat_bpb {
    uint8_t  jmp_boot[3];
    char     oem_id[8];           /* "EXFAT   " */
    uint8_t  reserved1[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;          /* in sectors */
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t fs_revision;
    uint16_t volume_flags;
    uint8_t  bytes_per_sector_shift;
    uint8_t  sectors_per_cluster_shift;
    uint8_t  number_of_fats;
    uint8_t  drive_select;
    uint8_t  percent_in_use;
    uint8_t  reserved2[7];
} __attribute__((packed));

/* Directory entry header (type 0x85) */
struct exfat_file_entry {
    uint8_t  type;
    uint8_t  secondary_count_continuations;
    uint16_t checksum;
    uint16_t file_attributes;
    uint16_t reserved1;
    uint32_t create_time;
    uint32_t modify_time;
    uint32_t access_time;
    uint8_t  create_time_ms;
    uint8_t  modify_time_ms;
    uint8_t  access_time_ms;
    uint8_t  reserved2[9];
} __attribute__((packed));

/* Stream extension entry (type 0xC0) */
struct exfat_stream_ext {
    uint8_t  type;
    uint8_t  general_secondary_flags;
    uint8_t  reserved1;
    uint8_t  name_length;
    uint16_t name_hash;
    uint16_t reserved2;
    uint64_t valid_data_length;
    uint32_t reserved3;
    uint32_t first_cluster;
    uint64_t data_length;
} __attribute__((packed));

/* File name entry (type 0xC1) */
struct exfat_file_name {
    uint8_t  type;
    uint8_t  general_secondary_flags;
    uint8_t  name[30];       /* UTF-16LE characters, 15 chars */
    uint8_t  reserved[2];
} __attribute__((packed));

/* Up-case table entry (type 0x82) */
struct exfat_upcase_entry {
    uint8_t  type;
    uint8_t  general_secondary_flags;
    uint8_t  reserved1[3];
    uint32_t checksum;
    uint32_t first_cluster;
    uint64_t data_length;
    uint8_t  reserved2[12];
} __attribute__((packed));

/* Allocation bitmap entry (type 0x81) */
struct exfat_bitmap_entry {
    uint8_t  type;
    uint8_t  general_secondary_flags;
    uint8_t  reserved1[3];
    uint32_t first_cluster;
    uint64_t data_length;
    uint8_t  reserved2[12];
} __attribute__((packed));

/* Volume label entry (type 0x83) */
struct exfat_label_entry {
    uint8_t  type;
    uint8_t  character_count;
    uint8_t  label[22];
    uint8_t  reserved[8];
} __attribute__((packed));

#pragma pack(pop)

/* Private mount data */
struct exfat_priv {
    uint8_t  dev_id;
    uint32_t bytes_per_sector;
    uint8_t  sectors_per_cluster_shift;
    uint32_t sector_size;
    uint32_t cluster_size;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint32_t volume_serial;
    uint16_t volume_flags;
    uint32_t num_clusters;
    uint32_t data_start_sector;   /* first sector of cluster heap */

    /* ── Allocation bitmap management ────────────────────────────── */
    uint32_t bitmap_start_sector;   /* first sector of the allocation bitmap */
    uint32_t bitmap_sectors;        /* number of sectors in the bitmap */
    uint32_t next_free_hint;        /* cluster hint for next allocation scan */
    uint32_t free_clusters;         /* running count of free clusters */
    uint32_t cached_bitmap_sector;  /* bitmap sector in cache (~0 = invalid) */
    uint8_t  cached_bitmap_data[512]; /* cached bitmap sector data */
    int      cached_bitmap_dirty;   /* 1 = cache needs write-back */
    int      bitmap_initialized;    /* 1 = bitmap has been initialized */

    /* ── FAT table management ──────────────────────────────────── */
    uint32_t cached_fat_sector;     /* FAT sector in cache (~0 = invalid) */
    uint8_t  cached_fat_data[512];  /* cached FAT sector data */
    int      cached_fat_dirty;      /* 1 = cache needs write-back */
};

int exfat_probe(uint8_t dev_id);
int exfat_init(void);
#endif
