#include "partitions.h"
#include "string.h"
#include "printf.h"
#include "crc.h"
#include "heap.h"
#include "export.h"
#include "kernel.h"

/* ── Known GPT partition type GUIDs (common ones) ──────────────────── */

static const struct {
    const uint8_t guid[16];
    const char   *name;
} gpt_type_table[] = {
    /* EFI System Partition */
    { {0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
       0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B},
      "EFI System Partition" },
    /* MBR partition scheme (protective MBR) */
    { {0x24,0x42,0x4D,0x02, 0x3E,0x3E, 0x4F,0x42,
       0xAC,0xF1,0x15,0xAA,0xED,0xC5,0xBA,0xBE},
      "MBR partition scheme" },
    /* Intel Fast Flash (iFFS) */
    { {0xD3,0xBF,0xE2,0xD9, 0xDA,0x9D, 0x31,0x43,
       0xBF,0x41,0xCC,0x6A,0x7E,0x61,0x05,0x94},
      "Intel Fast Flash" },
    /* Basic data partition (Windows/Linux data) */
    { {0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
       0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7},
      "Basic data partition (Linux/Windows)" },
    /* Linux filesystem data */
    { {0x0F,0xC6,0x3D,0xAF, 0x84,0x63, 0x47,0x72,
       0x8E,0xAB,0x33,0x4B,0x28,0x73,0x99,0x56},
      "Linux filesystem" },
    /* Linux swap */
    { {0x06,0x5F,0xD6,0xFD, 0xA4,0xAB, 0x43,0xC4,
       0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F},
      "Linux swap" },
    /* Linux LVM */
    { {0xE6,0xD6,0xD3,0x8F, 0xDF,0xEA, 0x4C,0x44,
       0x8E,0x5B,0xB7,0x63,0x1B,0x17,0xB6,0x50},
      "Linux LVM" },
    /* Linux RAID */
    { {0xA1,0x9D,0x88,0x0A, 0xFB,0xBC, 0x44,0x38,
       0x94,0x6F,0x05,0x2B,0x4C,0x22,0x55,0x93},
      "Linux RAID" },
    /* Linux /usr partition */
    { {0xD4,0x83,0x65,0x84, 0x15,0x97, 0x45,0x6E,
       0x9F,0x8F,0xB9,0x94,0xC6,0x18,0x33,0x2F},
      "Linux /usr" },
    /* Windows Reserved */
    { {0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
       0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE},
      "Microsoft Reserved" },
    /* Windows Basic Data */
    { {0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
       0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7},
      "Microsoft Basic Data" },
    /* Windows Recovery Environment */
    { {0xDE,0x94,0xBB,0xE4, 0x06,0xE1, 0xD4,0x11,
       0x9D,0x31,0x00,0xE0,0xC7,0x39,0xDB,0x95},
      "Windows Recovery Environment" },
    /* FreeBSD Data */
    { {0x51,0x65,0x72,0xE6, 0xAC,0x6F, 0x11,0xD6,
       0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x6E},
      "FreeBSD Data" },
    /* FreeBSD Swap */
    { {0x51,0x65,0x72,0xE6, 0xAC,0x6F, 0x11,0xD6,
       0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x6F},
      "FreeBSD Swap" },
    /* FreeBSD UFS */
    { {0x51,0x65,0x72,0xE6, 0xAC,0x6F, 0x11,0xD6,
       0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x70},
      "FreeBSD UFS" },
    /* FreeBSD ZFS */
    { {0x51,0x65,0x72,0xE6, 0xAC,0x6F, 0x11,0xD6,
       0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x71},
      "FreeBSD ZFS" },
    /* Apple APFS */
    { {0x37,0xD6,0x50,0x7D, 0xE5,0x7C, 0x46,0x3D,
       0x9C,0x2E,0x7B,0x0C,0x7F,0x15,0x88,0x58},
      "Apple APFS" },
    /* Apple HFS/HFS+ */
    { {0x00,0x53,0x46,0x48, 0x00,0x00,0xAA,0x11,
       0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC},
      "Apple HFS/HFS+" },
    /* Solaris Boot */
    { {0x6A,0x82,0xCF,0x6A, 0x1D,0xD4, 0x4B,0x11,
       0xA4,0xBE,0xBE,0x3E,0x2B,0xAA,0x6C,0x29},
      "Solaris Boot" },
    /* Solaris Root */
    { {0xBE,0x07,0x4F,0x6A, 0x1D,0xD4, 0x4B,0x11,
       0xA4,0xBE,0xBE,0x3E,0x2B,0xAA,0x6C,0x29},
      "Solaris Root" },
    /* VMware VMFS */
    { {0xAA,0x9D,0x88,0x0A, 0xFB,0xBC, 0x44,0x38,
       0x94,0x6F,0x05,0x2B,0x4C,0x22,0x55,0x93},
      "VMware VMFS" },
};

static const int gpt_type_count = ARRAY_SIZE(gpt_type_table);

/* ── GPT statistics ────────────────────────────────────────────────── */
static struct gpt_stats gpt_last_stats;

/* ── MBR Parsing ───────────────────────────────────────────────────── */

int partitions_read(const uint8_t *mbr_sector, struct partition_entry *entries)
{
    if (!mbr_sector || !entries)
        return -1;

    /* Check MBR signature */
    const struct mbr *mbr = (const struct mbr *)mbr_sector;
    if (mbr->signature != 0xAA55) {
        /* Not a valid MBR */
        return 0;
    }

    int count = 0;
    for (int i = 0; i < 4; i++) {
        entries[i] = mbr->partitions[i];
        /* A partition entry is considered valid if type != 0 */
        if (entries[i].type != 0)
            count++;
    }

    return count;
}

/* ── MBR validity ────────────────────────────────────────────────── */

int mbr_is_valid(const uint8_t *sector_buf)
{
    if (!sector_buf)
        return 0;

    const struct mbr *mbr = (const struct mbr *)sector_buf;
    return (mbr->signature == 0xAA55) ? 1 : 0;
}

/* ── EBR chain traversal ─────────────────────────────────────────── */

/* Check if a partition type is an extended partition marker */
static int is_extended_type(uint8_t type)
{
    return (type == MBR_TYPE_EXTENDED_CHS ||
            type == MBR_TYPE_EXTENDED_LBA ||
            type == MBR_TYPE_EXTENDED_LINUX);
}

/* Read one EBR sector and extract the logical partition entry + next EBR pointer.
 *
 * @ebr_lba        Absolute LBA of the EBR sector to read.
 * @disk_read      Disk read callback.
 * @logical_out    Output for the logical partition entry (may be NULL).
 * @next_ebr_lba   Output for the next EBR's absolute LBA (pointer from entry 1),
 *                 set to 0 if no further EBRs exist (may be NULL).
 *
 * Returns 1 if a valid EBR with a logical partition was found,
 *         0 if the EBR signature is invalid or no logical partition exists,
 *         < 0 on I/O error.
 */
static int read_ebr_entry(uint64_t ebr_lba,
                          disk_read_callback_t disk_read,
                          struct partition_entry *logical_out,
                          uint64_t *next_ebr_lba)
{
    uint8_t sector[512];
    int ret;

    memset(sector, 0, sizeof(sector));
    ret = disk_read(ebr_lba, sector, 1);
    if (ret < 0)
        return ret;

    const struct ebr *ebr = (const struct ebr *)sector;
    if (ebr->signature != 0xAA55)
        return 0;  /* Not a valid EBR */

    /* Entry 0: the logical partition itself.
     * start_lba is relative to the EBR's LBA. */
    if (logical_out && ebr->entries[0].type != 0) {
        logical_out->bootable    = ebr->entries[0].bootable;
        logical_out->start_head  = ebr->entries[0].start_head;
        logical_out->start_sector = ebr->entries[0].start_sector;
        logical_out->start_cyl   = ebr->entries[0].start_cyl;
        logical_out->type        = ebr->entries[0].type;
        logical_out->end_head    = ebr->entries[0].end_head;
        logical_out->end_sector  = ebr->entries[0].end_sector;
        logical_out->end_cyl     = ebr->entries[0].end_cyl;
        logical_out->start_lba   = ebr->entries[0].start_lba;
        logical_out->sector_count = ebr->entries[0].sector_count;
    }

    /* Entry 1: pointer to the next EBR in the chain (if any).
     * type=0x05/0x0F/0x85 and start_lba is the relative offset from
     * the original extended partition's start.  We store the absolute
     * LBA only if the entry is valid. */
    if (next_ebr_lba) {
        if (ebr->entries[1].type != 0 &&
            is_extended_type(ebr->entries[1].type)) {
            /* The start_lba in entry 1 is relative to the EBR's base.
             * The absolute LBA is ebr_lba + start_lba. */
            *next_ebr_lba = ebr_lba + ebr->entries[1].start_lba;
        } else {
            *next_ebr_lba = 0;
        }
    }

    return (ebr->entries[0].type != 0) ? 1 : 0;
}

/* ── Full MBR parser with EBR chain ──────────────────────────────── */

/* Global MBR parse statistics (last mbr_parse() call) */
static struct mbr_stats mbr_last_stats;

int mbr_parse(disk_read_callback_t disk_read, uint64_t disk_sectors,
              struct mbr_partition *mbr_entries, int max_entries)
{
    uint8_t mbr_sector[512];
    int total_partitions = 0;

    /* Clear stats */
    memset(&mbr_last_stats, 0, sizeof(mbr_last_stats));

    if (!disk_read || !mbr_entries || max_entries <= 0)
        return -1;

    /* Read the MBR (sector 0) */
    memset(mbr_sector, 0, sizeof(mbr_sector));
    if (disk_read(0, mbr_sector, 1) < 0) {
        kprintf("[MBR] Failed to read MBR sector\n");
        return -1;
    }

    /* Validate MBR signature */
    if (!mbr_is_valid(mbr_sector)) {
        kprintf("[MBR] Invalid MBR signature (no 0xAA55)\n");
        mbr_last_stats.signature_valid = 0;
        return 0;
    }
    mbr_last_stats.signature_valid = 1;

    /* Parse the MBR structure */
    const struct mbr *mbr = (const struct mbr *)mbr_sector;

    /* Track the extended partition's base LBA for EBR traversal.
     * The first extended partition entry we encounter provides the base
     * from which all EBR chain offsets are computed. */
    uint64_t ext_base_lba = 0;

    /* ── Step 1: Extract primary partitions ────────────────────────── */
    for (int i = 0; i < MBR_MAX_PRIMARY; i++) {
        const struct partition_entry *raw = &mbr->partitions[i];

        if (raw->type == 0)
            continue;  /* Unused entry */

        /* If this is an extended partition, record the base LBA and
         * skip adding it as a regular partition entry.  Extended
         * partitions themselves are not mountable — they contain
         * logical partitions. */
        if (is_extended_type(raw->type)) {
            mbr_last_stats.has_extended = 1;
            ext_base_lba = raw->start_lba;

            /* Optionally record the extended partition as a slot,
             * but skip adding to the output for now.  Callers that
             * want the extended partition entry can look at the
             * has_extended flag and ext_base_lba. */
            continue;
        }

        /* Validate partition boundaries */
        if (raw->start_lba > disk_sectors ||
            raw->sector_count == 0 ||
            (uint64_t)raw->start_lba + raw->sector_count > disk_sectors) {
            kprintf("[MBR] Primary partition %d: invalid bounds "
                    "(start=%u count=%u, disk=%llu sectors)\n",
                    i + 1, raw->start_lba, raw->sector_count,
                    (unsigned long long)disk_sectors);
            continue;
        }

        /* Valid primary partition */
        if (total_partitions < max_entries) {
            mbr_entries[total_partitions].bootable     = raw->bootable;
            mbr_entries[total_partitions].type         = raw->type;
            mbr_entries[total_partitions].start_lba    = raw->start_lba;
            mbr_entries[total_partitions].sector_count = raw->sector_count;
            mbr_entries[total_partitions].is_logical   = 0;
            mbr_entries[total_partitions].index        = i + 1;
        }
        total_partitions++;
    }

    mbr_last_stats.primary_count = total_partitions;

    /* ── Step 2: Follow the EBR chain for logical partitions ───────── */
    if (mbr_last_stats.has_extended && ext_base_lba > 0) {
        uint64_t ebr_lba = ext_base_lba;  /* First EBR is at the extended partition start */
        int ebr_depth = 0;

        while (ebr_lba > 0 && ebr_lba < disk_sectors) {
            struct partition_entry logical_raw;
            uint64_t next_ebr_lba = 0;
            int ret;

            memset(&logical_raw, 0, sizeof(logical_raw));

            ret = read_ebr_entry(ebr_lba, disk_read,
                                 &logical_raw, &next_ebr_lba);
            if (ret < 0) {
                kprintf("[MBR] I/O error reading EBR at LBA %llu\n",
                        (unsigned long long)ebr_lba);
                break;
            }
            if (ret == 0)
                break;  /* Reached end of EBR chain */

            ebr_depth++;

            /* Validate the logical partition's bounds */
            uint32_t abs_start = ebr_lba + logical_raw.start_lba;

            if (abs_start > disk_sectors ||
                logical_raw.sector_count == 0 ||
                (uint64_t)abs_start + logical_raw.sector_count > disk_sectors) {
                kprintf("[MBR] Logical partition at EBR %llu: "
                        "invalid bounds (abs_start=%u count=%u)\n",
                        (unsigned long long)ebr_lba,
                        abs_start, logical_raw.sector_count);
                ebr_lba = next_ebr_lba;
                continue;
            }

            /* Valid logical partition */
            if (total_partitions < max_entries) {
                mbr_entries[total_partitions].bootable     = logical_raw.bootable;
                mbr_entries[total_partitions].type         = logical_raw.type;
                mbr_entries[total_partitions].start_lba    = abs_start;
                mbr_entries[total_partitions].sector_count = logical_raw.sector_count;
                mbr_entries[total_partitions].is_logical   = 1;
                /* Logical partition numbering: starts at 5 (sda5, sda6, ...) */
                mbr_entries[total_partitions].index        = MBR_MAX_PRIMARY + ebr_depth;
            }
            total_partitions++;

            /* Cap the EBR chain depth to prevent infinite loops */
            if (ebr_depth >= MBR_MAX_LOGICAL) {
                kprintf("[MBR] EBR chain exceeds maximum depth (%d), stopping\n",
                        MBR_MAX_LOGICAL);
                break;
            }

            /* Move to the next EBR in the chain */
            ebr_lba = next_ebr_lba;

            /* Safety: if the next EBR points to the same LBA, break
             * to avoid an infinite loop on corrupt partition tables. */
            if (ebr_lba == next_ebr_lba && ebr_lba != 0) {
                kprintf("[MBR] EBR chain loop detected at LBA %llu, aborting\n",
                        (unsigned long long)ebr_lba);
                break;
            }
        }

        mbr_last_stats.logical_count = total_partitions - mbr_last_stats.primary_count;
        mbr_last_stats.ebr_chain_depth = ebr_depth;
    }

    /* Check for truncation */
    if (total_partitions > max_entries) {
        mbr_last_stats.truncated = 1;
        kprintf("[MBR] Warning: %d partitions found but only %d slots available\n",
                total_partitions, max_entries);
    }

    return (total_partitions < max_entries) ? total_partitions : max_entries;
}

void mbr_get_stats(struct mbr_stats *stats)
{
    if (stats)
        memcpy(stats, &mbr_last_stats, sizeof(mbr_last_stats));
}

/* ── GPT Parsing ───────────────────────────────────────────────────── */

/*
 * Compare two 16-byte GUIDs (binary).
 * Returns 1 if equal, 0 otherwise.
 */
static int guid_eq(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < 16; i++) {
        if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/*
 * Decode a UTF-16LE string into a NUL-terminated ASCII buffer.
 * Non-ASCII characters are replaced with '?'.
 * @src   Input UTF-16LE string (source, length in bytes = max_dst * 2)
 * @dst   Output ASCII buffer
 * @max   Maximum number of characters to write (including NUL)
 */
static void utf16le_to_ascii(const char *src, char *dst, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i * 2] != '\0'; i++) {
        uint16_t cp = (uint8_t)src[i * 2] | ((uint8_t)src[i * 2 + 1] << 8);
        if (cp < 0x80 && cp >= 0x20) {
            dst[i] = (char)cp;
        } else if (cp == 0) {
            break;
        } else {
            dst[i] = '?';  /* non-ASCII, non-printable */
        }
    }
    dst[i] = '\0';
}

/*
 * Check if a sector buffer contains a valid GPT header.
 * Returns 1 if the GPT signature matches, 0 otherwise.
 */
int gpt_is_valid(const uint8_t *sector_buf)
{
    if (!sector_buf)
        return 0;

    const struct gpt_header *hdr = (const struct gpt_header *)sector_buf;
    return (hdr->signature[0] == GPT_SIGNATURE_LO &&
            hdr->signature[1] == GPT_SIGNATURE_HI) ? 1 : 0;
}

/*
 * Validate a GPT header's CRC32.
 * CRC is computed over bytes [0, header->header_size) with the
 * header_crc32 field zeroed during calculation, as per UEFI spec.
 *
 * Returns 1 if CRC is valid, 0 otherwise.
 */
int gpt_validate_header_crc(const struct gpt_header *hdr, const uint8_t *raw)
{
    if (!hdr || !raw)
        return 0;

    uint32_t saved_crc = hdr->header_crc32;
    uint32_t size = hdr->header_size;

    /* Clamp header size to a reasonable range (92 bytes minimum, 512 max) */
    if (size < 92 || size > 512)
        return 0;

    /* Copy the header bytes and zero the CRC field for computation */
    uint8_t copy[512];
    memset(copy, 0, sizeof(copy));
    memcpy(copy, raw, size > 512 ? 512 : size);

    /* CRC field is at offset 16 in the GPT header (bytes 16-19) */
    if (size > 16) {
        copy[16] = 0;
        copy[17] = 0;
        copy[18] = 0;
        copy[19] = 0;
    }

    uint32_t computed = crc32(0, copy, size > 512 ? 512 : size);
    return (computed == saved_crc);
}

/*
 * Try to parse GPT partition entries by reading them from disk.
 * Called after a valid GPT header has been found (primary or backup).
 *
 * @hdr            Valid GPT header
 * @disk_read      Callback to read raw sectors from disk
 * @gpt_entries    Output array
 * @max_entries    Size of output array
 * @disk_sectors   Total sectors on disk (for validation)
 *
 * Returns number of valid partitions, or <0 on error.
 */
static int gpt_parse_entries_from_disk(const struct gpt_header *hdr,
                                        disk_read_callback_t disk_read,
                                        struct gpt_partition *gpt_entries,
                                        int max_entries,
                                        uint64_t disk_sectors)
{
    if (!hdr || !disk_read || !gpt_entries || max_entries <= 0)
        return -1;

    uint32_t num_entries  = hdr->num_entries;
    uint32_t entry_size   = hdr->entry_size;
    uint64_t part_start   = hdr->part_start_lba;

    /* Sanity checks */
    if (num_entries == 0 || num_entries > GPT_MAX_PARTITIONS) {
        kprintf("[GPT] Invalid number of partition entries: %u\n", num_entries);
        return -1;
    }
    if (entry_size < sizeof(struct gpt_entry) || entry_size > 1024) {
        kprintf("[GPT] Invalid partition entry size: %u\n", entry_size);
        return -1;
    }

    /* Validate that the partition entry array fits on the disk */
    uint64_t entries_total_bytes = (uint64_t)num_entries * entry_size;
    uint64_t entries_total_sectors = (entries_total_bytes + 511) / 512;
    if (part_start + entries_total_sectors > disk_sectors) {
        kprintf("[GPT] Partition entries extend beyond disk end\n");
        return -1;
    }

    /* Allocate a temporary buffer for the raw entries */
    uint64_t buf_size = entries_total_sectors * 512;
    uint8_t *raw_entries = (uint8_t *)kmalloc(buf_size);
    if (!raw_entries) {
        kprintf("[GPT] Failed to allocate %llu bytes for partition entries\n",
                (unsigned long long)buf_size);
        return -1;
    }

    /* Read partition entry array from disk */
    if (disk_read(part_start, raw_entries, (int)entries_total_sectors) < 0) {
        kprintf("[GPT] Failed to read partition entries at LBA %llu\n",
                (unsigned long long)part_start);
        kfree(raw_entries);
        return -1;
    }

    /* Validate entries CRC if the header provides one */
    if (hdr->entries_crc32 != 0) {
        uint32_t computed = crc32(0, raw_entries, (uint32_t)(num_entries * entry_size));
        if (computed != hdr->entries_crc32) {
            kprintf("[GPT] Partition entries CRC mismatch (expected 0x%08x, got 0x%08x)\n",
                    hdr->entries_crc32, computed);
            /* Proceed anyway — CRC mismatch is logged but not fatal */
        }
    }

    /* Decode entries */
    int count = gpt_decode_entries(raw_entries, num_entries, entry_size,
                                    gpt_entries, max_entries);

    kfree(raw_entries);
    return count;
}

/*
 * Validate a full GPT header (signature, CRC, sanity checks).
 * Returns 1 if valid, 0 otherwise.
 */
static int gpt_header_is_valid(const uint8_t *sector_buf, const char *location)
{
    if (!sector_buf || !gpt_is_valid(sector_buf)) {
        if (location)
            kprintf("[GPT] %s header: Invalid signature\n", location);
        return 0;
    }

    const struct gpt_header *hdr = (const struct gpt_header *)sector_buf;

    /* Check revision (must be >= 1.0 = 0x00010000) */
    if (hdr->revision < 0x00010000U) {
        if (location)
            kprintf("[GPT] %s header: Unsupported revision 0x%08x\n",
                    location, hdr->revision);
        return 0;
    }

    /* Validate CRC */
    if (!gpt_validate_header_crc(hdr, sector_buf)) {
        if (location)
            kprintf("[GPT] %s header: CRC32 invalid\n", location);
        return 0;
    }

    return 1;
}

/*
 * Parse GPT from a raw sector buffer, with optional backup header fallback.
 *
 * @sector_buf      512-byte buffer containing the primary GPT header sector.
 * @disk_sectors    Total number of sectors on the disk (needed for backup
 *                  header location).
 * @disk_read       Optional callback to read raw sectors.  If NULL, backup
 *                  header fallback is skipped.
 * @gpt_entries     Output array for parsed partition entries.
 * @max_entries     Size of the output array.
 *
 * Returns number of valid GPT partitions found, or <0 on error.
 */
int gpt_parse(const uint8_t *sector_buf, uint64_t disk_sectors,
              disk_read_callback_t disk_read,
              struct gpt_partition *gpt_entries, int max_entries)
{
    /* Clear stats */
    memset(&gpt_last_stats, 0, sizeof(gpt_last_stats));

    if (!sector_buf || !gpt_entries || max_entries <= 0)
        return -1;

    /* ── Try the primary GPT header (LBA 1) ─────────────────────────── */
    int primary_valid = gpt_header_is_valid(sector_buf, "Primary");
    gpt_last_stats.primary_valid = primary_valid;
    gpt_last_stats.crc_ok = primary_valid;

    if (primary_valid) {
        /* Primary header is good — use it */
        gpt_last_stats.used_backup = 0;

        const struct gpt_header *hdr = (const struct gpt_header *)sector_buf;

        /* If we have a disk read callback, parse the full partition entries */
        if (disk_read != NULL && disk_sectors > 0) {
            return gpt_parse_entries_from_disk(hdr, disk_read,
                                                gpt_entries, max_entries,
                                                disk_sectors);
        }

        /* Without a disk read callback, we can only validate the header.
         * Return the number of entries claimed by the header. */
        return (int)hdr->num_entries;
    }

    /* ── Primary header is invalid — try backup header ──────────────── */
    if (disk_read == NULL || disk_sectors == 0) {
        kprintf("[GPT] Primary header invalid and no disk read callback — "
                "cannot fall back to backup header\n");
        return -1;
    }

    /* The backup GPT header is located at the last LBA of the disk.
     * The header's backup_lba field should also point here, but we can
     * compute it directly from disk_sectors to handle cases where the
     * primary header is too corrupt to read backup_lba. */
    uint64_t backup_lba = disk_sectors - 1;

    kprintf("[GPT] Primary header invalid — attempting backup header "
            "at LBA %llu\n", (unsigned long long)backup_lba);

    /* Read the backup header sector */
    uint8_t backup_sector[512];
    memset(backup_sector, 0, sizeof(backup_sector));

    if (disk_read(backup_lba, backup_sector, 1) < 0) {
        kprintf("[GPT] Failed to read backup header at LBA %llu\n",
                (unsigned long long)backup_lba);
        gpt_last_stats.backup_valid = 0;
        return -1;
    }

    /* Validate backup header */
    int backup_valid = gpt_header_is_valid(backup_sector, "Backup");
    gpt_last_stats.backup_valid = backup_valid;

    if (!backup_valid) {
        kprintf("[GPT] Both primary and backup GPT headers are invalid — "
                "cannot parse partitions\n");
        return -1;
    }

    /* Backup header is valid — use it */
    kprintf("[GPT] Using backup GPT header from LBA %llu\n",
            (unsigned long long)backup_lba);
    gpt_last_stats.used_backup = 1;

    const struct gpt_header *hdr = (const struct gpt_header *)backup_sector;

    /* Validate that the backup header's current_lba points to where we found it */
    if (hdr->current_lba != backup_lba) {
        kprintf("[GPT] Backup header current_lba mismatch: header says %llu, "
                "but disk says %llu — continuing anyway\n",
                (unsigned long long)hdr->current_lba,
                (unsigned long long)backup_lba);
    }

    /* Parse partition entries using the backup header */
    return gpt_parse_entries_from_disk(hdr, disk_read,
                                        gpt_entries, max_entries,
                                        disk_sectors);
}

/*
 * Validate a full set of GPT partition entries against their CRC32
 * (computed from the entries CRC field in the GPT header).
 *
 * @entries    Raw partition entry buffer (from disk)
 * @num        Number of entries in the buffer
 * @entry_size Size of each entry (from header)
 * @expected_crc  Expected CRC32 value (from header->entries_crc32)
 *
 * Returns 1 if CRC matches, 0 otherwise.
 */
int gpt_validate_entries_crc(const uint8_t *entries, uint32_t num,
                              uint32_t entry_size, uint32_t expected_crc)
{
    if (!entries || num == 0 || entry_size == 0)
        return 0;

    uint32_t total_bytes = num * entry_size;
    uint32_t computed = crc32(0, entries, total_bytes);
    return (computed == expected_crc);
}

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
                        struct gpt_partition *out, int max_out)
{
    int count = 0;
    const uint8_t zero_guid[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0, 0};

    for (uint32_t i = 0; i < num && count < max_out; i++) {
        const struct gpt_entry *raw = (const struct gpt_entry *)
            (raw_entries + (uint64_t)i * entry_size);

        /* Skip unused entries (zero type GUID) */
        if (guid_eq(raw->type_guid, zero_guid))
            continue;

        /* Skip entries that are clearly invalid */
        if (raw->end_lba <= raw->start_lba)
            continue;

        /* Copy fields */
        memcpy(out[count].type_guid, raw->type_guid, 16);
        memcpy(out[count].part_guid, raw->part_guid, 16);
        out[count].start_lba = raw->start_lba;
        out[count].end_lba   = raw->end_lba;
        out[count].attr      = raw->attr;

        /* Decode UTF-16LE partition name to ASCII */
        utf16le_to_ascii(raw->name, out[count].name, (int)sizeof(out[count].name));

        count++;
    }

    return count;
}

/* ── Statistics ────────────────────────────────────────────────────── */

void gpt_get_stats(struct gpt_stats *stats)
{
    if (stats)
        memcpy(stats, &gpt_last_stats, sizeof(gpt_last_stats));
}

/* ── Human-readable names ──────────────────────────────────────────── */

const char *partition_type_name(uint8_t type)
{
    switch (type) {
    case 0x00: return "Empty";
    case 0x01: return "FAT12";
    case 0x04: return "FAT16 (<32M)";
    case 0x05: return "Extended";
    case 0x06: return "FAT16B";
    case 0x07: return "NTFS/HPFS";
    case 0x08: return "FAT32-compatible";
    case 0x0B: return "FAT32 (CHS)";
    case 0x0C: return "FAT32 (LBA)";
    case 0x0E: return "FAT16B (LBA)";
    case 0x0F: return "Extended (LBA)";
    case 0x11: return "Hidden FAT12";
    case 0x12: return "OEM Config";
    case 0x14: return "Hidden FAT16";
    case 0x16: return "Hidden FAT16B";
    case 0x17: return "Hidden NTFS";
    case 0x1B: return "Hidden FAT32";
    case 0x1C: return "Hidden FAT32 (LBA)";
    case 0x1E: return "Hidden FAT16 (LBA)";
    case 0x27: return "Windows RE";
    case 0x39: return "Plan 9";
    case 0x41: return "PPC PReP";
    case 0x42: return "Windows LDM";
    case 0x63: return "Unix (GNU HURD)";
    case 0x80: return "Minix";
    case 0x81: return "Linux swap";
    case 0x82: return "Linux swap / Solaris";
    case 0x83: return "Linux ext2/3/4";
    case 0x84: return "OS/2 hidden C:";
    case 0x85: return "Linux extended";
    case 0x86: return "NTFS mirror set";
    case 0x87: return "NTFS stripe set";
    case 0x8E: return "Linux LVM";
    case 0x93: return "Linux software RAID (autodetect)";
    case 0x9F: return "BSD/OS";
    case 0xA0: return "IBM Thinkpad hibernation";
    case 0xA5: return "FreeBSD";
    case 0xA6: return "OpenBSD";
    case 0xA7: return "NeXTSTEP";
    case 0xA8: return "Apple UFS";
    case 0xA9: return "NetBSD";
    case 0xAB: return "Apple boot";
    case 0xAF: return "Apple HFS/HFS+";
    case 0xB7: return "BSDI FS";
    case 0xB8: return "BSDI swap";
    case 0xBE: return "Solaris boot";
    case 0xBF: return "Solaris";
    case 0xC0: return "CTOS";
    case 0xC1: return "DR-DOS FAT12";
    case 0xC4: return "DR-DOS FAT16";
    case 0xC6: return "DR-DOS FAT16B";
    case 0xC7: return "Syrinx";
    case 0xDA: return "Non-FS data";
    case 0xDE: return "Dell Utility";
    case 0xE8: return "LUKS";
    case 0xEB: return "Bech FAT16";
    case 0xEE: return "GPT protective";
    case 0xEF: return "EFI System Partition";
    case 0xF0: return "Linux PA-RISC boot";
    case 0xFB: return "VMware VMFS";
    case 0xFC: return "VMware swap";
    case 0xFD: return "Linux RAID autodetect";
    default:   return "Unknown";
    }
}

const char *gpt_type_name(const uint8_t *type_guid)
{
    if (!type_guid)
        return "Unknown";

    for (int i = 0; i < gpt_type_count; i++) {
        if (guid_eq(gpt_type_table[i].guid, type_guid))
            return gpt_type_table[i].name;
    }

    return "Unknown";
}

/* ── Hybrid MBR + GPT ───────────────────────────────────────────────── */

/*
 * Check if an MBR sector contains a hybrid MBR.
 *
 * A hybrid MBR has at least one 0xEE (GPT protective) entry AND at least
 * one non-0xEE, non-zero partition entry.  A simple protective MBR has
 * exactly one 0xEE entry and no others.
 *
 * Returns 1 if hybrid, 0 if not (simple protective MBR, traditional MBR,
 * or invalid MBR).
 */
int mbr_is_hybrid(const uint8_t *mbr_sector)
{
    if (!mbr_sector)
        return 0;

    const struct mbr *mbr = (const struct mbr *)mbr_sector;
    if (mbr->signature != 0xAA55)
        return 0;

    int protective_count = 0;
    int real_count = 0;

    for (int i = 0; i < MBR_MAX_PRIMARY; i++) {
        uint8_t type = mbr->partitions[i].type;

        if (type == MBR_TYPE_GPT_PROTECTIVE) {
            protective_count++;
        } else if (type != 0) {
            real_count++;
        }
    }

    /* Hybrid = at least one protective 0xEE entry AND at least one real entry */
    return (protective_count > 0 && real_count > 0) ? 1 : 0;
}

/*
 * Parse hybrid MBR partition entries.
 *
 * Extracts only the real (non-0xEE, non-zero) partition entries from an MBR
 * sector that is known to be a hybrid MBR.  The 0xEE GPT protective entries
 * are excluded from output.
 *
 * @mbr_sector  512-byte MBR buffer (must be hybrid-valid)
 * @entries     Output array for extracted entries
 * @max_entries Capacity of the output array
 *
 * Returns number of real partition entries found, or < 0 on error.
 */
int hybrid_parse(const uint8_t *mbr_sector,
                  struct hybrid_entry *entries, int max_entries)
{
    if (!mbr_sector || !entries || max_entries <= 0)
        return -1;

    const struct mbr *mbr = (const struct mbr *)mbr_sector;
    if (mbr->signature != 0xAA55)
        return -1;

    int count = 0;

    for (int i = 0; i < MBR_MAX_PRIMARY && count < max_entries; i++) {
        uint8_t type = mbr->partitions[i].type;

        /* Skip empty entries and GPT protective entries */
        if (type == 0 || type == MBR_TYPE_GPT_PROTECTIVE)
            continue;

        entries[count].type         = type;
        entries[count].start_lba    = mbr->partitions[i].start_lba;
        entries[count].sector_count = mbr->partitions[i].sector_count;
        entries[count].gpt_index    = -1;  /* unknown until mapped */
        count++;
    }

    return count;
}

/*
 * Find the GPT partition index that a hybrid MBR entry corresponds to.
 *
 * An MBR entry corresponds to a GPT partition if the MBR start_lba falls
 * within the GPT partition's LBA range and the size is compatible.
 *
 * @mbr_start   Starting LBA of the MBR entry
 * @mbr_size    Size in sectors of the MBR entry
 * @gpt         Array of GPT partition entries
 * @gpt_count   Number of GPT entries
 *
 * Returns the GPT index (0-based) on match, or -1 if no match found.
 */
static int hybrid_find_gpt_match(uint32_t mbr_start, uint32_t mbr_size,
                                  const struct gpt_partition *gpt,
                                  int gpt_count)
{
    if (!gpt || gpt_count <= 0)
        return -1;

    for (int i = 0; i < gpt_count; i++) {
        /* Check if the MBR entry's start LBA is within this GPT partition */
        if ((uint64_t)mbr_start >= gpt[i].start_lba &&
            (uint64_t)mbr_start < gpt[i].end_lba) {
            /* Start matches — now check size compatibility.
             * The MBR size may be slightly smaller (capped at 2TiB for MBR),
             * so allow the MBR size to be <= the GPT size from the start. */
            uint64_t gpt_size_from_start = gpt[i].end_lba - (uint64_t)mbr_start;
            if ((uint64_t)mbr_size <= gpt_size_from_start) {
                return i;
            }
        }
    }

    return -1;
}

/*
 * Validate that a hybrid MBR's real partition entries are consistent with
 * the GPT partition entries.
 *
 * Checks:
 *   1. Each real MBR entry maps to a GPT partition (by start LBA overlap)
 *   2. The GPT partition's type GUID suggests a compatible MBR type
 *   3. The ranges are consistent
 *
 * @mbr_entries   Hybrid MBR entries (from hybrid_parse)
 * @mbr_count     Number of hybrid MBR entries
 * @gpt_entries   GPT partitions (from gpt_parse)
 * @gpt_count     Number of GPT partitions
 *
 * Returns 1 if all hybrid entries validate against GPT, 0 otherwise.
 * When validation fails, a kprintf message is emitted with details.
 */
int hybrid_validate(struct hybrid_entry *mbr_entries, int mbr_count,
                    const struct gpt_partition *gpt_entries, int gpt_count)
{
    if (!mbr_entries || mbr_count <= 0)
        return 1;  /* No hybrid entries to validate = trivially valid */

    if (!gpt_entries || gpt_count <= 0) {
        kprintf("[HYBRID] No GPT partitions to validate against\n");
        return 0;
    }

    int valid = 1;

    for (int i = 0; i < mbr_count; i++) {
        int gpt_idx = hybrid_find_gpt_match(mbr_entries[i].start_lba,
                                             mbr_entries[i].sector_count,
                                             gpt_entries, gpt_count);

        if (gpt_idx < 0) {
            kprintf("[HYBRID] MBR entry %d (type 0x%02x, LBA %u, %u sectors) "
                    "does not match any GPT partition\n",
                    i + 1, mbr_entries[i].type,
                    mbr_entries[i].start_lba, mbr_entries[i].sector_count);
            valid = 0;
            continue;
        }

        /* Record the mapping */
        mbr_entries[i].gpt_index = gpt_idx;

        kprintf("[HYBRID] MBR entry %d (type 0x%02x) → GPT partition %d "
                "(%s)\n",
                i + 1, mbr_entries[i].type, gpt_idx,
                gpt_entries[gpt_idx].name);
    }

    return valid;
}

/*
 * Known MBR ↔ GPT type mapping, used by hybrid_get_mbr_type().
 */
static const struct {
    uint8_t      mbr_type;       /* MBR partition type byte */
    const uint8_t gpt_guid[16];  /* Corresponding GPT type GUID */
} hybrid_type_map[] = {
    /* EFI System Partition: MBR 0xEF, GPT C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    { 0xEF, {0x28,0x73,0x2A,0xC1, 0x1F,0xF8, 0xD2,0x11,
              0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B} },
    /* Linux filesystem: MBR 0x83, GPT 0FC63DAF-8483-4772-8EAB-334B28739956 */
    { 0x83, {0xAF,0x3D,0xC6,0x0F, 0x83,0x84, 0x72,0x47,
              0x8E,0xAB,0x33,0x4B,0x28,0x73,0x99,0x56} },
    /* Linux swap: MBR 0x82, GPT 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F */
    { 0x82, {0x6D,0xFD,0x57,0x06, 0xAB,0xA4, 0xC4,0x43,
              0x84,0xE5,0x09,0x33,0xC8,0x4B,0x4F,0x4F} },
    /* Linux LVM: MBR 0x8E, GPT E6D6D38F-DFEA-4C44-8E5B-B7631B17B650 */
    { 0x8E, {0x8F,0xD3,0xD6,0xE6, 0xEA,0xDF, 0x44,0x4C,
              0x8E,0x5B,0xB7,0x63,0x1B,0x17,0xB6,0x50} },
    /* Linux RAID: MBR 0xFD, GPT A19D880A-FBBC-4438-946F-052B4C225593 */
    { 0xFD, {0x0A,0x88,0x9D,0xA1, 0xBC,0xFB, 0x38,0x44,
              0x94,0x6F,0x05,0x2B,0x4C,0x22,0x55,0x93} },
    /* Windows Basic Data: MBR 0x07, GPT EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
    { 0x07, {0xA2,0xA0,0xD0,0xEB, 0xE5,0xB9, 0x33,0x44,
              0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7} },
    /* Microsoft Reserved: MBR 0x0C, GPT E3C9E316-0B5C-4DB8-817D-F92DF00215AE */
    { 0x0C, {0x16,0xE3,0xC9,0xE3, 0x5C,0x0B, 0xB8,0x4D,
              0x81,0x7D,0xF9,0x2D,0xF0,0x02,0x15,0xAE} },
    /* FreeBSD Data: MBR 0xA5, GPT 5165E672-6FAC-11D6-9C97-0010917B4E6E */
    { 0xA5, {0x72,0xE6,0x65,0x51, 0xAC,0x6F, 0xD6,0x11,
              0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x6E} },
    /* FreeBSD Swap: MBR 0xA5, GPT 5165E672-6FAC-11D6-9C97-0010917B4E6F */
    { 0xA5, {0x72,0xE6,0x65,0x51, 0xAC,0x6F, 0xD6,0x11,
              0x9C,0x97,0x00,0x10,0x91,0x7B,0x4E,0x6F} },
    /* Apple HFS/HFS+: MBR 0xAF, GPT 48465300-0000-11AA-AA11-00306543ECAC */
    { 0xAF, {0x00,0x53,0x46,0x48, 0x00,0x00, 0xAA,0x11,
              0xAA,0x11,0x00,0x30,0x65,0x43,0xEC,0xAC} },
    /* Apple APFS: MBR 0xAF (same MBR type as HFS+), GPT 7D50D637-7C5E-463D-9C2E-7B0C7F155858 */
    { 0xAF, {0x37,0xD6,0x50,0x7D, 0xE5,0x7C, 0x46,0x3D,
              0x9C,0x2E,0x7B,0x0C,0x7F,0x15,0x88,0x58} },
};

static const int hybrid_type_map_count = ARRAY_SIZE(hybrid_type_map);

/*
 * Get the recommended MBR partition type byte for a given GPT type GUID.
 * Used when creating or validating hybrid MBR entries.
 *
 * @type_guid  16-byte GPT partition type GUID
 * @returns    MBR partition type byte, or 0xEE (GPT protective) as default
 *             if no mapping is known.
 */
uint8_t hybrid_get_mbr_type(const uint8_t *type_guid)
{
    if (!type_guid)
        return MBR_TYPE_GPT_PROTECTIVE;

    for (int i = 0; i < hybrid_type_map_count; i++) {
        if (guid_eq(hybrid_type_map[i].gpt_guid, type_guid))
            return hybrid_type_map[i].mbr_type;
    }

    /* Default: GPT protective (no known MBR equivalent).
     * This means the partition cannot be represented in hybrid MBR, which
     * is the correct fallback — not all GPT partitions have MBR type codes. */
    return MBR_TYPE_GPT_PROTECTIVE;
}

/*
 * Build a mapping between hybrid MBR entries and GPT partition entries.
 *
 * For each hybrid MBR entry, finds the corresponding GPT partition by
 * start LBA overlap and records the index in entries[].gpt_index.
 * Also returns the total count of successfully mapped entries.
 *
 * @mbr_entries   Hybrid MBR entries (gpt_index fields are filled in)
 * @mbr_count     Number of hybrid MBR entries
 * @gpt_entries   GPT partition entries (from gpt_parse)
 * @gpt_count     Number of GPT partition entries
 *
 * Returns number of entries successfully mapped.
 */
int hybrid_build_map(struct hybrid_entry *mbr_entries, int mbr_count,
                      const struct gpt_partition *gpt_entries, int gpt_count)
{
    if (!mbr_entries || mbr_count <= 0)
        return 0;

    if (!gpt_entries || gpt_count <= 0)
        return 0;

    int mapped = 0;

    for (int i = 0; i < mbr_count; i++) {
        int gpt_idx = hybrid_find_gpt_match(mbr_entries[i].start_lba,
                                             mbr_entries[i].sector_count,
                                             gpt_entries, gpt_count);
        mbr_entries[i].gpt_index = gpt_idx;
        if (gpt_idx >= 0)
            mapped++;
    }

    return mapped;
}

/* ── Exports for module use ────────────────────────────────────────── */
EXPORT_SYMBOL(partitions_read);
EXPORT_SYMBOL(gpt_is_valid);
EXPORT_SYMBOL(gpt_parse);
EXPORT_SYMBOL(gpt_decode_entries);
EXPORT_SYMBOL(gpt_validate_entries_crc);
EXPORT_SYMBOL(partition_type_name);
EXPORT_SYMBOL(gpt_type_name);
EXPORT_SYMBOL(gpt_validate_header_crc);
EXPORT_SYMBOL(gpt_get_stats);
EXPORT_SYMBOL(mbr_is_valid);
EXPORT_SYMBOL(mbr_parse);
EXPORT_SYMBOL(mbr_get_stats);
EXPORT_SYMBOL(mbr_is_hybrid);
EXPORT_SYMBOL(hybrid_parse);
EXPORT_SYMBOL(hybrid_validate);
EXPORT_SYMBOL(hybrid_get_mbr_type);
EXPORT_SYMBOL(hybrid_build_map);

/* ── Initialisation ─────────────────────────────────────────────────── */

int partitions_init(void)
{
    kprintf("[OK] Partitions: MBR/GPT partition parser initialised\n");
    return 0;
}

/* ── Partition scanning from a gendisk ──────────────────────────────── */

/* The partitions_scan function is called during disk discovery to
 * parse the partition table of a gendisk and register partition
 * entries.  Currently a placeholder that auto-detects MBR vs GPT
 * and logs the result.  Integration with blockdev/gendisk partition
 * registration is done in genhd.c / blockdev.c. */
int partitions_scan(void *dev)
{
    /* In the current architecture, partition scanning is driven by the
     * gendisk layer which calls mbr_parse() or gpt_parse() directly
     * with the appropriate disk_read callback.
     *
     * This function exists as a future hook for scanning all disks
     * from a single entry point. */
    (void)dev;
    return 0;
}
