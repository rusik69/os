#include "partitions.h"
#include "string.h"
#include "printf.h"
#include "crc.h"
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
static int gpt_validate_header_crc(const struct gpt_header *hdr, const uint8_t *raw)
{
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
 * Parse GPT partition entries from a sector buffer.
 * Returns the number of valid entries found, or <0 on error.
 */
int gpt_parse(const uint8_t *sector_buf, uint64_t disk_sectors,
              struct gpt_partition *gpt_entries, int max_entries)
{
    if (!sector_buf || !gpt_entries || max_entries <= 0)
        return -1;

    const struct gpt_header *primary = (const struct gpt_header *)sector_buf;

    /* Check GPT signature */
    if (!gpt_is_valid(sector_buf))
        return -1;

    /* Validate header CRC32 */
    if (!gpt_validate_header_crc(primary, sector_buf)) {
        kprintf("[GPT] Primary header CRC32 invalid\n");
        return -1;
    }

    /* Check revision (must be >= 1.0 = 0x00010000) */
    if (primary->revision < 0x00010000U) {
        kprintf("[GPT] Unsupported revision 0x%08x\n", primary->revision);
        return -1;
    }

    uint32_t num_entries  = primary->num_entries;
    uint32_t entry_size   = primary->entry_size;

    /* Sanity-check partition entry parameters */
    if (num_entries == 0 || num_entries > GPT_MAX_PARTITIONS) {
        kprintf("[GPT] Invalid number of partition entries: %u\n", num_entries);
        return -1;
    }
    if (entry_size < sizeof(struct gpt_entry) || entry_size > 1024) {
        kprintf("[GPT] Invalid partition entry size: %u\n", entry_size);
        return -1;
    }

    /* We don't have the partition entries array here (it spans multiple sectors
     * starting at part_start_lba).  This function currently parses only the
     * header sector itself.  For a full parse, the caller must read the
     * partition entry array using the part_start_lba and num_entries fields.
     *
     * For now, we validate that the header looks correct and return a count
     * based on what the header claims.  A future enhancement could accept
     * the full partition entry array buffer. */

    /* Return the number of entries claimed by the header so the caller knows
     * how many to read.  We can't validate individual entries without the
     * actual entry data. */
    (void)disk_sectors;  /* not yet used; future use for backup header */

    return (int)num_entries;
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
