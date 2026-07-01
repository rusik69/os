#ifndef PARTITIONS_H
#define PARTITIONS_H

#include "types.h"

/* ── MBR constants ─────────────────────────────────────────────────── */

/* Maximum number of primary MBR partitions */
#define MBR_MAX_PRIMARY       4

/* Maximum number of logical partitions through EBR chain */
#define MBR_MAX_LOGICAL       64

/* Size of raw MBR partition entry in bytes */
#define MBR_PART_ENTRY_SIZE   16

/* Known MBR partition types for extended partitions */
#define MBR_TYPE_EXTENDED_CHS    0x05  /* Extended partition (CHS) */
#define MBR_TYPE_EXTENDED_LBA    0x0F  /* Extended partition (LBA) */
#define MBR_TYPE_EXTENDED_LINUX  0x85  /* Linux extended */
#define MBR_TYPE_GPT_PROTECTIVE  0xEE  /* GPT protective MBR */

/* MBR partition entry flags */
#define MBR_BOOTABLE            0x80   /* Active / bootable flag */

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

/* EBR (Extended Boot Record) structure — same layout as MBR but only the
 * first two partition entries are meaningful:
 *   Entry 0 = logical partition (type, start, size relative to EBR LBA)
 *   Entry 1 = next EBR pointer (type=0x05/0x0F, start = offset to next EBR)
 * Signature at offset 0x1FE = 0xAA55 */
struct ebr {
    uint8_t  padding[446];
    struct partition_entry entries[2];
    uint16_t signature;
} __attribute__((packed));

/* Higher-level MBR partition descriptor (like gpt_partition for GPT).
 * Contains all information from the raw entry plus derived fields. */
struct mbr_partition {
    uint8_t   bootable;       /* 0x80 = active, 0x00 = inactive */
    uint8_t   type;           /* partition type byte */
    uint32_t  start_lba;      /* absolute start LBA */
    uint32_t  sector_count;   /* size in sectors */
    int       is_logical;     /* 1 = logical partition (from EBR), 0 = primary */
    int       index;          /* partition number (1-based, e.g. sda1 → 1) */
};

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

/* ── Disk read callback ────────────────────────────────────────────────
 * Callback used by GPT parsing to read raw sectors from a block device.
 * @lba           Start LBA to read from.
 * @buf           Output buffer (must be at least sector_count * 512 bytes).
 * @sector_count  Number of 512-byte sectors to read.
 *
 * Returns 0 on success, negative on error.
 */
typedef int (*disk_read_callback_t)(uint64_t lba, void *buf, int sector_count);

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * Parse MBR primary partitions from a 512-byte MBR sector buffer.
 *
 * @mbr_sector  512-byte buffer containing the MBR (sector 0)
 * @entries     Output array for up to 4 primary partition entries
 *
 * Returns the number of valid (non-zero type) primary partitions found,
 * or 0 if the MBR signature is invalid.
 */
int partitions_read(const uint8_t *mbr_sector, struct partition_entry *entries);

/*
 * Check whether a 512-byte sector buffer contains a valid MBR.
 * Returns 1 if the MBR signature (0xAA55) is present, 0 otherwise.
 */
int mbr_is_valid(const uint8_t *sector_buf);

/*
 * Parse MBR partitions from a disk, including EBR-based logical partitions.
 *
 * This is the MBR equivalent of gpt_parse().  It reads the MBR sector
 * via @disk_read, validates it, extracts primary partitions, then
 * follows the EBR chain to discover logical partitions.
 *
 * @disk_read        Callback to read raw sectors from the disk.
 *                   Must not be NULL.
 * @disk_sectors     Total number of sectors on the disk
 *                   (used to validate partition boundaries).
 * @mbr_entries      Output array for parsed MBR partitions
 *                   (up to MBR_MAX_PRIMARY + MBR_MAX_LOGICAL).
 * @max_entries      Size of the output array.
 *
 * Returns number of valid partitions found (primaries + logicals),
 * or < 0 on error.
 */
int mbr_parse(disk_read_callback_t disk_read, uint64_t disk_sectors,
              struct mbr_partition *mbr_entries, int max_entries);

/*
 * Parse MBR from an in-memory buffer (no disk read).
 * Convenience wrapper for callers that already have the MBR sector.
 * Only primary partitions are returned (no EBR chain traversal).
 *
 * @mbr_sector  512-byte buffer containing the MBR
 * @mbr_entries Output array for parsed MBR partitions
 * @max_entries Size of the output array
 *
 * Returns number of valid partitions found, or < 0 on error.
 */
static inline int mbr_parse_from_buf(const uint8_t *mbr_sector,
                                     struct mbr_partition *mbr_entries,
                                     int max_entries)
{
    /* Without a disk read callback we cannot traverse EBR chains,
     * so only parse the 4 primary entries from the buffer. */
    (void)max_entries;
    struct partition_entry raw[MBR_MAX_PRIMARY];
    int count = partitions_read(mbr_sector, raw);
    if (count <= 0)
        return count;

    for (int i = 0; i < count && i < MBR_MAX_PRIMARY; i++) {
        mbr_entries[i].bootable     = raw[i].bootable;
        mbr_entries[i].type         = raw[i].type;
        mbr_entries[i].start_lba    = raw[i].start_lba;
        mbr_entries[i].sector_count = raw[i].sector_count;
        mbr_entries[i].is_logical   = 0;
        mbr_entries[i].index        = i + 1;
    }
    return count;
}

/*
 * Parse GPT from a raw sector buffer (primary header expected at LBA 1).
 *
 * If the primary header CRC is invalid, attempts to read and validate the
 * backup GPT header from the last LBA of the disk.  The backup header is
 * read via the optional @disk_read callback.
 *
 * @sector_buf      512-byte buffer containing the primary GPT header sector.
 * @disk_sectors    Total number of sectors on the disk (needed for backup
 *                  header location and validation).
 * @disk_read       Optional callback to read raw sectors from the disk.
 *                  If NULL, backup header fallback is skipped and only the
 *                  primary header is used.
 * @gpt_entries     Output array for parsed partition entries
 *                  (up to GPT_MAX_PARTITIONS).
 * @max_entries     Size of the output array.
 *
 * Returns number of valid GPT partitions found, or <0 on error.
 */
int gpt_parse(const uint8_t *sector_buf, uint64_t disk_sectors,
              disk_read_callback_t disk_read,
              struct gpt_partition *gpt_entries, int max_entries);

/*
 * Parse GPT using only an in-memory header buffer (no disk read fallback).
 * Convenience wrapper for callers that have already read the header sector.
 *
 * @sector_buf      512-byte buffer containing the GPT header sector.
 * @gpt_entries     Output array for parsed partition entries
 *                  (up to GPT_MAX_PARTITIONS).
 * @max_entries     Size of the output array.
 *
 * Returns number of valid GPT partitions found, or <0 on error.
 */
static inline int gpt_parse_from_buf(const uint8_t *sector_buf,
                                     struct gpt_partition *gpt_entries,
                                     int max_entries) {
    return gpt_parse(sector_buf, 0, NULL, gpt_entries, max_entries);
}

/*
 * Check whether a sector buffer contains a valid GPT header.
 * Returns 1 if GPT signature matches, 0 otherwise.
 */
int gpt_is_valid(const uint8_t *sector_buf);

/* Validate a GPT header's CRC32.
 * Returns 1 if CRC is valid, 0 otherwise. */
int gpt_validate_header_crc(const struct gpt_header *hdr, const uint8_t *raw);

/*
 * Validate a full set of GPT partition entries against their CRC32.
 * Returns 1 if CRC matches, 0 otherwise.
 */
int gpt_validate_entries_crc(const uint8_t *entries, uint32_t num,
                              uint32_t entry_size, uint32_t expected_crc);

/*
 * Decode raw GPT partition entries (from disk) into higher-level
 * gpt_partition structures with ASCII names and validated content.
 *
 * @raw_entries  Raw bytes of the partition entry array (from disk)
 * @num          Number of entries to decode
 * @entry_size   Size of each entry in the raw buffer
 * @out          Output array of gpt_partition structures
 * @max_out      Size of the output array
 *
 * Returns number of partitions successfully decoded (non-zero type GUID).
 */
int gpt_decode_entries(const uint8_t *raw_entries, uint32_t num,
                        uint32_t entry_size,
                        struct gpt_partition *out, int max_out);

/* Get a human-readable name for a partition type. */
const char *partition_type_name(uint8_t type);

/* Get a human-readable name for a GPT partition type GUID.
 * Returns a static string or "Unknown". */
const char *gpt_type_name(const uint8_t *type_guid);

/* GPT parse statistics */
struct gpt_stats {
    int primary_valid;    /* 1 if primary header was valid */
    int backup_valid;     /* 1 if backup header was valid (0 if not checked) */
    int used_backup;      /* 1 if backup header was used for parsing */
    int crc_ok;           /* 1 if primary header CRC passed */
};

/* Get GPT parse statistics from the most recent gpt_parse() call. */
void gpt_get_stats(struct gpt_stats *stats);

/* ── MBR parse statistics ────────────────────────────────────────── */
struct mbr_stats {
    int primary_count;        /* number of primary partitions found */
    int logical_count;        /* number of logical partitions found */
    int has_extended;         /* 1 if an extended partition was present */
    int ebr_chain_depth;      /* length of EBR chain scanned */
    int signature_valid;      /* 1 if MBR signature 0xAA55 was present */
    int truncated;            /* 1 if result was truncated (max_entries) */
};

/* Get MBR parse statistics from the most recent mbr_parse() call. */
void mbr_get_stats(struct mbr_stats *stats);

#endif /* PARTITIONS_H */
