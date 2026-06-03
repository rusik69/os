#ifndef PARTITIONS_H
#define PARTITIONS_H

#include "types.h"

/* ── MBR structures (Legacy DOS partition table) ───────────────────── */

/* MBR partition table entry (16 bytes) */
struct partition_entry {
    uint8_t  bootable;       /* 0x80 = bootable */
    uint8_t  start_head;
    uint8_t  start_sector;   /* bits 0-5: sector, bits 6-7: high bits of cylinder */
    uint8_t  start_cyl;
    uint8_t  type;           /* partition type (e.g. 0x83 = Linux) */
    uint8_t  end_head;
    uint8_t  end_sector;
    uint8_t  end_cyl;
    uint32_t start_lba;      /* LBA of first sector */
    uint32_t sector_count;   /* number of sectors */
} __attribute__((packed));

/* MBR structure (sector 0, 512 bytes) */
struct mbr {
    uint8_t  bootstrap[446];
    struct partition_entry partitions[4];
    uint16_t signature;      /* must be 0xAA55 */
} __attribute__((packed));

/* ── GPT structures (GUID Partition Table) ─────────────────────────── */

/* GPT signature: "EFI PART" */
#define GPT_SIGNATURE_LO  0x54524150U  /* "PART" */
#define GPT_SIGNATURE_HI  0x20494645U  /* "EFI " */

/* GUID partition entry (128 bytes) */
struct gpt_entry {
    uint8_t  type_guid[16];    /* Partition type GUID */
    uint8_t  part_guid[16];    /* Unique partition GUID */
    uint64_t start_lba;        /* First LBA */
    uint64_t end_lba;          /* Last LBA (inclusive) */
    uint64_t attr;             /* Attribute flags */
    char     name[72];         /* UTF-16LE partition name (36 chars) */
} __attribute__((packed));

/* GPT header (92 bytes, usually padded to sector size) */
struct gpt_header {
    uint64_t signature[2];     /* GPT_SIGNATURE_LO, GPT_SIGNATURE_HI */
    uint32_t revision;         /* GPT revision (1.0 = 0x00010000) */
    uint32_t header_size;      /* Size in bytes (usually 92) */
    uint32_t header_crc32;     /* CRC32 of header (offset 0 to header_size) */
    uint32_t reserved1;
    uint64_t current_lba;      /* LBA of this header (usually 1) */
    uint64_t backup_lba;       /* LBA of backup header (usually last LBA) */
    uint64_t first_usable;     /* First usable LBA for partitions */
    uint64_t last_usable;      /* Last usable LBA for partitions */
    uint8_t  disk_guid[16];    /* Disk GUID */
    uint64_t part_start_lba;   /* Starting LBA of partition entries array */
    uint32_t num_entries;      /* Number of partition entries */
    uint32_t entry_size;       /* Size of each partition entry (usually 128) */
    uint32_t entries_crc32;    /* CRC32 of partition entries array */
} __attribute__((packed));

/* GPT partition entry for higher-level consumption (flat, ASCII name) */
struct gpt_partition {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t start_lba;
    uint64_t end_lba;
    uint64_t attr;
    char     name[72];         /* Decoded ASCII name (from UTF-16LE) */
};

#define GPT_MAX_PARTITIONS 128

/* ── Public API ────────────────────────────────────────────────────── */

/* Parse MBR from a 512-byte buffer. Fills entries array (up to 4).
   Returns number of valid (non-zero type) partitions found. */
int partitions_read(const uint8_t *mbr_sector, struct partition_entry *entries);

/*
 * Parse GPT from a raw sector buffer (primary header expected at LBA 1).
 *
 * Reads the GPT header and validates it (signature, revision, CRC32).
 * If the primary header is invalid, attempts to read the backup header
 * from the last LBA of the disk (backup_lba field).
 *
 * @sector_buf  512-byte buffer containing the GPT header sector
 * @disk_sectors  Total number of sectors on the disk (needed for backup header LBA)
 * @gpt_entries  Output array for parsed partition entries (up to GPT_MAX_PARTITIONS)
 * @max_entries  Size of the output array
 *
 * Returns number of valid GPT partitions found, or <0 on error.
 */
int gpt_parse(const uint8_t *sector_buf, uint64_t disk_sectors,
              struct gpt_partition *gpt_entries, int max_entries);

/*
 * Check whether a sector buffer contains a valid GPT header.
 * Returns 1 if GPT signature matches, 0 otherwise.
 */
int gpt_is_valid(const uint8_t *sector_buf);

/* Get a human-readable name for a partition type. */
const char *partition_type_name(uint8_t type);

/* Get a human-readable name for a GPT partition type GUID.
 * Returns a static string or "Unknown". */
const char *gpt_type_name(const uint8_t *type_guid);

#endif /* PARTITIONS_H */
