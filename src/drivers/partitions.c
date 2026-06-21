#include "partitions.h"
#include "string.h"
#include "printf.h"
#include "crc.h"
#include "heap.h"
#include "export.h"

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

static const int gpt_type_count = sizeof(gpt_type_table) / sizeof(gpt_type_table[0]);

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

/* ── Stub: partitions_init ─────────────────────────────── */
int partitions_init(void)
{
    kprintf("[partitions] partitions_init: not yet implemented\n");
    return 0;
}
/* ── Stub: partitions_scan ─────────────────────────────── */
int partitions_scan(void *dev)
{
    (void)dev;
    kprintf("[partitions] partitions_scan: not yet implemented\n");
    return 0;
}
