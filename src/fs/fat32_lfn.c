/*
 * fat32_lfn.c — VFAT Long File Name create + delete
 *
 * Provides VFAT (Long File Name) directory entry creation and deletion
 * for FAT12/16/32 filesystems. These functions operate on in-memory
 * directory sector buffers; the caller is responsible for all I/O.
 *
 * VFAT LFN entries precede the corresponding 8.3 directory entry.
 * Each LFN entry holds up to 13 UTF-16LE characters; multi-entry
 * chains are stored in reverse physical order (highest ordinal first).
 *
 * References:
 *   Microsoft FAT32 Specification (Microsoft Extensible Firmware Initiative)
 *   VFAT Long File Name specification (Windows 95 onward)
 */

#include "types.h"
#include "fat32.h"
#include "string.h"
#include "printf.h"
#include "errno.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define FAT32_ATTR_LFN        0x0F   /* all of RO|HIDDEN|SYSTEM|VOLUME_ID */
#define FAT32_ATTR_VOLUME_ID  0x08
#define FAT32_ATTR_DIRECTORY  0x10
#define FAT32_ATTR_ARCHIVE    0x20

#define LFN_LAST_FLAG         0x40   /* bit 6 set = last LFN entry in chain */
#define LFN_ORD_MASK          0x1F   /* bits 0-4 = ordinal number */
#define LFN_MAX_ENTRIES       20     /* max 20 LFN entries per file (255 chars) */
#define LFN_CHARS_PER_ENTRY   13     /* 13 UTF-16LE chars per LFN entry */
#define LFN_MAX_CHARS         255    /* max filename length per VFAT spec */
#define SECT_SIZE             512

/* ── VFAT LFN directory entry (raw 32-byte layout) ─────────────────── */

struct vfat_lfn {
    uint8_t  order;           /* ordinal (1-20), bit 6 = LAST_LFN marker */
    uint16_t name1[5];        /* characters 1-5 (UTF-16LE) */
    uint8_t  attr;            /* must be FAT32_ATTR_LFN (0x0F) */
    uint8_t  type;            /* must be 0 for VFAT LFN */
    uint8_t  checksum;        /* checksum of the 8.3 short name */
    uint16_t name2[6];        /* characters 6-11 (UTF-16LE) */
    uint16_t cluster;         /* must be 0 for LFN entries */
    uint16_t name3[2];        /* characters 12-13 (UTF-16LE) */
} __attribute__((packed));

/* ── 8.3 directory entry (for reference) ───────────────────────────── */

struct vfat_dirent {
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

/* ── vfat_checksum ───────────────────────────────────────────────────
 *
 * Compute the 11-byte checksum used by VFAT LFN to associate each
 * LFN entry with its corresponding 8.3 short directory entry.
 *
 * @name83_8:  8-byte short name (space-padded, NOT null-terminated)
 * @name83_3:  3-byte extension (space-padded, NOT null-terminated)
 *
 * Returns the 8-bit checksum.
 */
uint8_t vfat_checksum(const char name83_8[8], const char name83_3[3])
{
    uint8_t sum = 0;
    for (int i = 0; i < 8; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83_8[i]);
    for (int i = 0; i < 3; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)name83_3[i]);
    return sum;
}

/* ── vfat_needs_lfn ──────────────────────────────────────────────────
 *
 * Determine whether a filename requires VFAT long filename entries.
 * Returns 1 if LFN entries are needed, 0 if the name fits cleanly into
 * an 8.3 short name without case information loss.
 *
 * Criteria (any of these triggers LFN):
 *   - Name part longer than 8 characters
 *   - Extension longer than 3 characters
 *   - Contains lowercase letters (would lose case in 8.3)
 *   - Contains characters invalid in 8.3 names (spaces, +, ,, ;, =, etc.)
 */
int vfat_needs_lfn(const char *name)
{
    const char *dot;
    int name_len;
    int ext_len;

    if (!name || !*name)
        return 0;

    /* Find the LAST dot (VFAT convention: extension is after last dot) */
    dot = NULL;
    {
        const char *p = name;
        while (*p) {
            if (*p == '.')
                dot = p;
            p++;
        }
    }

    if (dot && dot > name) {
        name_len = (int)(dot - name);
        ext_len = (int)strlen(dot + 1);
    } else {
        name_len = (int)strlen(name);
        ext_len = 0;
    }

    /* Check name length */
    if (name_len > 8) return 1;

    /* Check extension length */
    if (ext_len > 3) return 1;

    /* Check for lowercase characters in name */
    for (int i = 0; i < name_len; i++) {
        if (name[i] >= 'a' && name[i] <= 'z')
            return 1;
    }

    /* Check for lowercase characters in extension */
    if (dot) {
        for (int i = 0; dot[1 + i]; i++) {
            if (dot[1 + i] >= 'a' && dot[1 + i] <= 'z')
                return 1;
        }
    }

    /* Check for characters invalid in 8.3 short names */
    for (int i = 0; name[i]; i++) {
        char c = name[i];
        if (c == '.') continue;               /* dot separator is handled */
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= 'a' && c <= 'z') continue;
        if (c >= '0' && c <= '9') continue;
        /* Valid special characters for 8.3: $ % ' - _ @ ~ ` ! ( ) ^ # & */
        if (c == '$' || c == '%' || c == '\'') continue;
        if (c == '-' || c == '_' || c == '@') continue;
        if (c == '~' || c == '`' || c == '!') continue;
        if (c == '(' || c == ')' || c == '^') continue;
        if (c == '#' || c == '&') continue;
        /* Any other character is invalid in 8.3 → needs LFN */
        return 1;
    }

    return 0;
}

/* ── vfat_count_lfn_entries ──────────────────────────────────────────
 *
 * Count the number of VFAT LFN directory entries needed to store the
 * given filename. Each entry holds up to 13 characters.
 *
 * Returns the entry count (0-20), or 20 if the name is longer than
 * LFN_MAX_CHARS = 255 characters (truncated to the VFAT spec limit).
 */
int vfat_count_lfn_entries(const char *name)
{
    int len;

    if (!name) return 0;
    len = (int)strlen(name);
    if (len <= 0) return 0;
    if (len > LFN_MAX_CHARS)
        len = LFN_MAX_CHARS;

    return (len + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY;
}

/* ── vfat_build_entry ────────────────────────────────────────────────
 *
 * Fill a single VFAT LFN directory entry structure (raw 32-byte buffer)
 * with the character data from 'name' starting at offset 'name_offset'.
 *
 * @entry_out:    output buffer (must be 32 bytes)
 * @ordinal:      entry ordinal (1-based)
 * @is_last:      non-zero if this is the last (highest ordinal) entry
 * @name:         full long filename (null-terminated)
 * @name_offset:  character offset into name for this entry (0, 13, 26, ...)
 * @checksum:     VFAT checksum of the associated 8.3 short name
 */
void vfat_build_entry(void *entry_out, int ordinal, int is_last,
                       const char *name, int name_offset,
                       uint8_t checksum)
{
    struct vfat_lfn *entry = (struct vfat_lfn *)entry_out;
    uint16_t buf[LFN_CHARS_PER_ENTRY] = {0};
    int name_len;

    /* Zero-fill the entry */
    __builtin_memset(entry, 0, sizeof(struct vfat_lfn));

    /* Set order field: bits 0-4 = ordinal, bit 6 = LAST_LFN marker */
    entry->order = (uint8_t)(ordinal & LFN_ORD_MASK);
    if (is_last)
        entry->order |= LFN_LAST_FLAG;

    /* Set the LFN attribute marker */
    entry->attr = FAT32_ATTR_LFN;
    entry->type = 0;
    entry->cluster = 0;
    entry->checksum = checksum;

    /* Encode up to 13 characters into UTF-16LE buffers */
    name_len = (int)strlen(name);
    if (name_len > LFN_MAX_CHARS)
        name_len = LFN_MAX_CHARS;
    for (int i = 0; i < LFN_CHARS_PER_ENTRY; i++) {
        int idx = name_offset + i;
        if (idx < name_len) {
            /* ASCII → UTF-16LE (low byte only; no Unicode support needed) */
            buf[i] = (uint16_t)(uint8_t)name[idx];
        } else if (idx == name_len) {
            /* Null-terminate: first padding char is 0x0000 */
            buf[i] = 0x0000;
        } else {
            /* Subsequent padding chars are 0xFFFF */
            buf[i] = 0xFFFF;
        }
    }

    /* Copy into the three name fields */
    for (int i = 0; i < 5; i++)
        entry->name1[i] = buf[i];
    for (int i = 0; i < 6; i++)
        entry->name2[i] = buf[5 + i];
    for (int i = 0; i < 2; i++)
        entry->name3[i] = buf[11 + i];
}

/* ── vfat_insert_entries ─────────────────────────────────────────────
 *
 * Insert VFAT LFN directory entries into a sector buffer at the given
 * entry index. The entries are inserted BEFORE the entry at entry_idx,
 * shifting all subsequent entries down to make room.
 *
 * IMPORTANT: The caller must ensure there is enough room in the sector
 * for the LFN entries. This function does NOT write anything to disk;
 * it only modifies the provided buffer. If the LFN entries would exceed
 * the sector boundary, the caller must flush the current sector and
 * call again with the next sector buffer.
 *
 * @sector_buf:  512-byte directory sector buffer (modified in-place)
 * @entry_idx:   entry index within the sector where LFN entries are
 *               inserted. Updated to point past the inserted entries
 *               (i.e., where the 8.3 entry should go).
 * @leaf:        long filename (null-terminated)
 * @name83_8:    8-byte short name buffer (space-padded)
 * @name83_3:    3-byte extension buffer (space-padded)
 *
 * Returns 0 on success, -ENOSPC if not enough room in the sector.
 */
int vfat_insert_entries(uint8_t *sector_buf, int *entry_idx,
                         const char *leaf,
                         const char name83_8[8], const char name83_3[3])
{
    int num_entries;
    int n_per_sector;
    uint8_t checksum;
    struct vfat_dirent *dirents;

    if (!sector_buf || !entry_idx || !leaf || !name83_8 || !name83_3)
        return -EINVAL;

    num_entries = vfat_count_lfn_entries(leaf);
    if (num_entries <= 0 || num_entries > LFN_MAX_ENTRIES)
        return -EINVAL;

    n_per_sector = (int)(SECT_SIZE / sizeof(struct vfat_dirent));
    if (*entry_idx < 0 || *entry_idx > n_per_sector)
        return -EINVAL;

    /* Check that we have enough room */
    if (*entry_idx + num_entries > n_per_sector)
        return -ENOSPC;

    checksum = vfat_checksum(name83_8, name83_3);
    dirents = (struct vfat_dirent *)sector_buf;

    /* Shift existing entries down by num_entries slots */
    if (*entry_idx < n_per_sector - num_entries) {
        __builtin_memmove(&dirents[*entry_idx + num_entries],
                          &dirents[*entry_idx],
                          (size_t)(n_per_sector - *entry_idx - num_entries)
                              * sizeof(struct vfat_dirent));
    }

    /* Build and insert LFN entries (reverse order: last entry first) */
    for (int n = num_entries - 1; n >= 0; n--) {
        int ord = n + 1;
        int is_last = (n == num_entries - 1);
        int name_off = n * LFN_CHARS_PER_ENTRY;

        vfat_build_entry(&dirents[*entry_idx], ord, is_last,
                         leaf, name_off, checksum);
        (*entry_idx)++;
    }

    return 0;
}

/* ── vfat_is_lfn_entry ───────────────────────────────────────────────
 *
 * Check whether a 32-byte directory entry is a VFAT LFN entry.
 * LFN entries have attribute byte == 0x0F.
 *
 * Returns 1 if the entry is an LFN entry, 0 otherwise.
 */
int vfat_is_lfn_entry(const void *entry)
{
    const struct vfat_dirent *de = (const struct vfat_dirent *)entry;
    return (de->attr == FAT32_ATTR_LFN) ? 1 : 0;
}

/* ── vfat_is_deleted_entry ───────────────────────────────────────────
 *
 * Check whether a directory entry is marked as deleted (first byte 0xE5).
 * Returns 1 if deleted, 0 otherwise.
 */
int vfat_is_deleted_entry(const void *entry)
{
    const struct vfat_dirent *de = (const struct vfat_dirent *)entry;
    return ((uint8_t)de->name[0] == 0xE5) ? 1 : 0;
}

/* ── vfat_is_end_of_dir ──────────────────────────────────────────────
 *
 * Check whether a directory entry marks the end of the directory
 * (first byte 0x00). Returns 1 if end, 0 otherwise.
 */
int vfat_is_end_of_dir(const void *entry)
{
    const struct vfat_dirent *de = (const struct vfat_dirent *)entry;
    return (de->name[0] == 0x00) ? 1 : 0;
}

/* ── vfat_compare_83_name ────────────────────────────────────────────
 *
 * Compare a directory entry's 8.3 name with a reference (case-insensitive).
 * Returns 1 if they match, 0 otherwise.
 *
 * The reference can be in lowercase; this function converts both sides
 * to uppercase for comparison (matching FAT case-insensitive semantics).
 */
int vfat_compare_83_name(const void *entry,
                          const char name83_8[8], const char name83_3[3])
{
    const struct vfat_dirent *de = (const struct vfat_dirent *)entry;

    for (int i = 0; i < 8; i++) {
        char a = de->name[i];
        char b = name83_8[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    for (int i = 0; i < 3; i++) {
        char a = de->ext[i];
        char b = name83_3[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}

/* ── vfat_get_83_name ────────────────────────────────────────────────
 *
 * Extract the 8.3 name from a directory entry into output buffers.
 * Output buffers are space-padded to 8 and 3 bytes respectively.
 */
void vfat_get_83_name(const void *entry, char name83_8[8], char name83_3[3])
{
    const struct vfat_dirent *de = (const struct vfat_dirent *)entry;
    __builtin_memcpy(name83_8, de->name, 8);
    __builtin_memcpy(name83_3, de->ext, 3);
}

/* ── vfat_get_lfn_checksum ───────────────────────────────────────────
 *
 * Get the checksum from the first LFN entry in a chain.
 * All LFN entries in the same chain have the same checksum; this
 * returns it from the first one it finds.
 *
 * Returns the checksum, or 0 if the entry is not an LFN entry.
 */
uint8_t vfat_get_lfn_checksum(const void *entry)
{
    const struct vfat_lfn *lfn = (const struct vfat_lfn *)entry;
    if (lfn->attr != FAT32_ATTR_LFN)
        return 0;
    return lfn->checksum;
}

/* ── vfat_delete_by_checksum ─────────────────────────────────────────
 *
 * Given a sector buffer and a range of entry indices, mark all VFAT LFN
 * entries whose checksum matches the given 'checksum' as deleted (0xE5).
 * This is used when deleting a file that has LFN entries — the LFN
 * entries all share the same checksum as the 8.3 entry's name.
 *
 * @sector_buf:  512-byte directory sector buffer (modified in-place)
 * @start_idx:   starting entry index to scan (inclusive)
 * @end_idx:     ending entry index to scan (exclusive, i.e., scan up to
 *               end_idx - 1). Use -1 for "until end of sector".
 * @checksum:    the checksum to match
 *
 * Returns the number of entries deleted (0 if none found).
 */
int vfat_delete_by_checksum(uint8_t *sector_buf, int start_idx, int end_idx,
                             uint8_t checksum)
{
    int n_per_sector;
    struct vfat_dirent *dirents;
    int deleted = 0;

    if (!sector_buf) return 0;

    n_per_sector = (int)(SECT_SIZE / sizeof(struct vfat_dirent));
    if (start_idx < 0) start_idx = 0;
    if (end_idx < 0 || end_idx > n_per_sector) end_idx = n_per_sector;

    dirents = (struct vfat_dirent *)sector_buf;

    for (int i = start_idx; i < end_idx; i++) {
        if (dirents[i].attr != FAT32_ATTR_LFN)
            continue;
        if (vfat_get_lfn_checksum(&dirents[i]) == checksum) {
            dirents[i].name[0] = (char)0xE5; /* mark as deleted */
            deleted++;
        }
    }

    return deleted;
}

/* ── vfat_mark_entry_deleted ─────────────────────────────────────────
 *
 * Mark a single directory entry as deleted by setting its first byte
 * to 0xE5. Returns 0 on success.
 */
int vfat_mark_entry_deleted(void *entry)
{
    struct vfat_dirent *de = (struct vfat_dirent *)entry;
    de->name[0] = (char)0xE5;
    return 0;
}

/* ── vfat_build_83_name ──────────────────────────────────────────────
 *
 * Convert a long filename into its 8.3 short name representation.
 * The output buffers are space-padded to 8 and 3 bytes respectively.
 *
 * This mirrors the first-pass algorithm used by VFAT:
 *   1. Uppercase the name and extension
 *   2. Strip invalid 8.3 characters
 *   3. Truncate name to 8 chars, extension to 3 chars
 *   4. Ensure name is not empty (use '_' if empty)
 *
 * NOTE: This does NOT handle collision resolution (numeric tails ~N).
 * That must be done by the caller using fat32_generate_short_name or
 * similar collision-detection logic.
 *
 * @long_name:   the full filename
 * @out_name:    output buffer (8 bytes, space-padded)
 * @out_ext:     output buffer (3 bytes, space-padded)
 */
void vfat_build_83_name(const char *long_name, char out_name[8], char out_ext[3])
{
    const char *dot;
    int ni, ei;

    /* Initialize to space-padded */
    __builtin_memset(out_name, ' ', 8);
    __builtin_memset(out_ext, ' ', 3);

    if (!long_name || !*long_name) {
        out_name[0] = '_';
        return;
    }

    /* Find last dot */
    dot = NULL;
    {
        const char *p = long_name;
        while (*p) {
            if (*p == '.') dot = p;
            p++;
        }
    }

    /* ── Build name part (max 8 chars) ── */
    ni = 0;
    {
        const char *src = long_name;
        int max_chars = (dot && dot > long_name) ? (int)(dot - long_name)
                                                  : (int)strlen(long_name);
        for (int i = 0; i < max_chars && ni < 8; i++) {
            char c = src[i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            /* Skip dots and spaces in the name part */
            if (c == '.' || c == ' ') continue;
            /* Accept valid 8.3 chars */
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '$' || c == '%' || c == '\'' || c == '-' ||
                c == '_' || c == '@' || c == '~' || c == '`' ||
                c == '!' || c == '(' || c == ')' || c == '^' ||
                c == '#' || c == '&') {
                out_name[ni++] = c;
            } else {
                out_name[ni++] = '_';
            }
        }
    }

    /* Ensure name is not empty */
    if (ni == 0)
        out_name[0] = '_';

    /* ── Build extension part (max 3 chars) ── */
    ei = 0;
    if (dot && dot[1]) {
        for (int i = 0; dot[1 + i] && ei < 3; i++) {
            char c = dot[1 + i];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '$' || c == '%' || c == '\'' || c == '-' ||
                c == '_' || c == '@' || c == '~' || c == '`' ||
                c == '!' || c == '(' || c == ')' || c == '^' ||
                c == '#' || c == '&') {
                out_ext[ei++] = c;
            } else {
                out_ext[ei++] = '_';
            }
        }
    }
}

/* ── lfn_utf16_to_utf8 ──────────────────────────────────────────────────
 * Encode a single UTF-16 code point (BMP: U+0000–U+FFFF) to UTF-8
 * into the output buffer.  Uppercase ASCII (A–Z) is downcased for
 * readability (matching VFAT convention).  Surrogate code points
 * (U+D800–U+DFFF) are replaced with '_' since VFAT LFN does not use
 * surrogate pairs.
 *
 * Returns 1 on success, 0 if the result would exceed out_max bounds
 * (caller treats this as "done — buffer full").
 */
static int lfn_utf16_to_utf8(uint16_t c, char *out, int *pos, int out_max)
{
    if (c >= 0xD800 && c <= 0xDFFF)
        c = '_';   /* invalid surrogate → replacement */

    if (c < 0x80) {
        /* 1-byte UTF-8: 0xxxxxxx – ASCII subset */
        if (*pos + 1 >= out_max) return 0;
        out[(*pos)++] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
        return 1;
    } else if (c < 0x800) {
        /* 2-byte UTF-8: 110xxxxx 10xxxxxx */
        if (*pos + 2 >= out_max) return 0;
        out[(*pos)++] = (char)(0xC0 | (c >> 6));
        out[(*pos)++] = (char)(0x80 | (c & 0x3F));
        return 1;
    } else {
        /* 3-byte UTF-8: 1110xxxx 10xxxxxx 10xxxxxx */
        if (*pos + 3 >= out_max) return 0;
        out[(*pos)++] = (char)(0xE0 | (c >> 12));
        out[(*pos)++] = (char)(0x80 | ((c >> 6) & 0x3F));
        out[(*pos)++] = (char)(0x80 | (c & 0x3F));
        return 1;
    }
}

/* ── vfat_reconstruct_name ─────────────────────────────────────────────
 *
 * Reconstruct a long filename from VFAT LFN directory entries.
 * Validates entry ordering and builds the null-terminated name string.
 *
 * The entries array follows ordinal order: lowest ordinal first,
 * highest ordinal last (entries[0] = ordinal 1, entries[count-1] =
 * ordinal N).  This is the order produced by dir_find and other
 * callers which store LFN entries via ord-1 indexing.
 *
 * Characters are stored as UTF-16LE and converted to UTF-8 on output.
 * Only the BMP (U+0000–U+FFFF) is handled; surrogate pairs are replaced.
 *
 * @entries:    array of 'count' LFN directory entries (32 bytes each)
 * @count:      number of LFN entries (1-20)
 * @out:        output buffer for the reconstructed name
 * @out_max:    maximum output buffer size (including null terminator)
 *
 * Returns the length of the reconstructed name (excluding null),
 * or negative errno on error.
 */
int vfat_reconstruct_name(const void *entries, int count,
                           char *out, int out_max)
{
    const struct vfat_lfn *lfn = (const struct vfat_lfn *)entries;
    int pos = 0;

    if (!entries || !out || out_max <= 0)
        return -EINVAL;
    if (count < 1 || count > LFN_MAX_ENTRIES) {
        if (out_max > 0) out[0] = '\0';
        return -EINVAL;
    }

    /* Validate: the last entry (entries[count-1], the highest ordinal)
     * must have the LAST_LFN bit (0x40) set */
    if (!(lfn[count - 1].order & LFN_LAST_FLAG)) {
        if (out_max > 0) out[0] = '\0';
        return -EINVAL;
    }

    /* Verify contiguity: ordinal values should be 1..count without gaps.
     * Entries are in forward ordinal order (lowest first, highest last).
     * For 3 entries: lfn[0].order==1, lfn[1].order==2, lfn[2].order==3. */
    for (int i = 0; i < count; i++) {
        int actual_ord  = (int)(lfn[i].order & LFN_ORD_MASK);
        int expected_ord = i + 1; /* forward order: ordinal = index + 1 */
        if (actual_ord != expected_ord) {
            if (out_max > 0) out[0] = '\0';
            return -EINVAL;
        }
        /* Only the highest-ordered entry (entries[count-1]) should
         * have the 0x40 LAST_LFN bit */
        if ((lfn[i].order & LFN_LAST_FLAG) && i != count - 1) {
            if (out_max > 0) out[0] = '\0';
            return -EINVAL;
        }
    }

    /* Reconstruct: iterate from ordinal 1 (entries[0]) up to ordinal N
     * (entries[count-1]).  Characters are stored as UTF-16LE; convert
     * to UTF-8 on output with uppercase-to-lowercase for readability.
     *
     * Per the VFAT spec, the name is terminated by 0x0000 in the entry
     * character array; all subsequent slots are filled with 0xFFFF padding.
     * Stop immediately on either sentinel — the name is done. */
    for (int seq = 0; seq < count; seq++) {
        const struct vfat_lfn *e = &lfn[seq];
        for (int i = 0; i < 5; i++) {
            uint16_t c = e->name1[i];
            if (!c || c == 0xFFFF) goto done;
            if (!lfn_utf16_to_utf8(c, out, &pos, out_max)) goto done;
        }
        for (int i = 0; i < 6; i++) {
            uint16_t c = e->name2[i];
            if (!c || c == 0xFFFF) goto done;
            if (!lfn_utf16_to_utf8(c, out, &pos, out_max)) goto done;
        }
        for (int i = 0; i < 2; i++) {
            uint16_t c = e->name3[i];
            if (!c || c == 0xFFFF) goto done;
            if (!lfn_utf16_to_utf8(c, out, &pos, out_max)) goto done;
        }
    }
done:
    out[pos] = '\0';
    return pos;
}

/* ── vfat_reconstruct_name_checked ────────────────────────────────────
 *
 * Reconstruct a long filename from VFAT LFN entries, with checksum
 * validation against the associated 8.3 short name.
 *
 * This is equivalent to vfat_reconstruct_name() followed by checksum
 * verification, providing an extra integrity check against corrupted
 * or maliciously crafted directory entries.
 *
 * @entries:    array of 'count' LFN directory entries (32 bytes each)
 * @count:      number of LFN entries (1-20)
 * @name83_8:   8-byte short name (space-padded, NOT null-terminated)
 * @name83_3:   3-byte extension (space-padded, NOT null-terminated)
 * @out:        output buffer for the reconstructed name
 * @out_max:    maximum output buffer size (including null terminator)
 *
 * Returns the length of the reconstructed name on success,
 * -EILSEQ if the checksum does not match,
 * or another negative errno on other errors.
 */
int vfat_reconstruct_name_checked(const void *entries, int count,
                                   const char name83_8[8],
                                   const char name83_3[3],
                                   char *out, int out_max)
{
    const struct vfat_lfn *lfn = (const struct vfat_lfn *)entries;

    if (count <= 0) {
        /* No LFN entries — nothing to verify */
        if (out_max > 0) out[0] = '\0';
        return 0;
    }

    uint8_t computed = vfat_checksum(name83_8, name83_3);
    /* Verify ALL LFN entries in the set have the same checksum
     * matching the 8.3 short name.  Per VFAT spec, every LFN entry
     * for a given file must carry the same checksum value. */
    for (int i = 0; i < count; i++) {
        if (lfn[i].checksum != computed)
            return -EILSEQ;
    }

    return vfat_reconstruct_name(entries, count, out, out_max);
}

/*
 * ── Module metadata (when built as a module) ──────────────────────────
 * MODULE_LICENSE, MODULE_AUTHOR, etc. are defined by the compilation
 * unit that pulls in this file as part of the fat32 module (fat32.c).
 */

/* ── fat32_lfn_selftest ──────────────────────────────────────────────
 *
 * Quick self-test of basic VFAT LFN functions. Returns 0 on success.
 * This can be called during driver init to verify core logic.
 */
__attribute__((unused))
static int fat32_lfn_selftest(void)
{
    uint8_t sector[SECT_SIZE];
    char n8[8], n3[3];
    int entry_idx;
    int ret;

    /* Test: checksum */
    {
        char test_name[8] = "HELLO   ";
        char test_ext[3] = "TXT";
        uint8_t ck = vfat_checksum(test_name, test_ext);
        if (ck == 0) return -1; /* checksum must be non-zero for non-empty */
    }

    /* Test: needs_lfn with short name */
    if (vfat_needs_lfn("hello.txt")) return -2;

    /* Test: needs_lfn with long name */
    if (!vfat_needs_lfn("HelloWorld.txt")) return -3;

    /* Test: count entries */
    {
        int cnt = vfat_count_lfn_entries("HelloWorld.txt");
        if (cnt != 1) return -4; /* "HelloWorld.txt" = 14 chars, needs 1 entry */
    }

    /* Test: insert entries */
    {
        char test_n8[8];
        char test_n3[3];

        __builtin_memset(sector, 0, sizeof(sector));
        vfat_build_83_name("HelloWorld.txt", test_n8, test_n3);

        /* Verify the 8.3 name was built */
        {
            int ok = 1;
            for (int i = 0; i < 8; i++)
                if (test_n8[i] != "HELLOWOR"[i]) ok = 0;
            if (!ok) return -5;
        }

        entry_idx = 0;
        ret = vfat_insert_entries(sector, &entry_idx,
                                   "HelloWorld.txt", test_n8, test_n3);
        if (ret != 0) return -6;

        /* Verify the entry at entry_idx should be the right position */
        if (entry_idx != 1) return -7; /* one LFN entry was inserted */

        /* Verify it's a valid LFN entry */
        if (!vfat_is_lfn_entry(&sector[0])) return -8;
    }

    /* Test: delete by checksum */
    {
        char test_n8[8] = "HELLOWOR";
        char test_n3[3] = "TXT";
        uint8_t ck = vfat_checksum(test_n8, test_n3);

        /* Build a sector with an LFN entry followed by an 8.3 entry */
        __builtin_memset(sector, 0, sizeof(sector));
        entry_idx = 0;
        ret = vfat_insert_entries(sector, &entry_idx,
                                   "HelloWorld.txt", test_n8, test_n3);
        if (ret != 0) return -9;

        /* Write a fake 8.3 entry after the LFN entries */
        {
            struct vfat_dirent *de;
            de = (struct vfat_dirent *)&sector[entry_idx * sizeof(struct vfat_dirent)];
            __builtin_memcpy(de->name, test_n8, 8);
            __builtin_memcpy(de->ext, test_n3, 3);
        }

        /* Delete LFN entries matching the checksum */
        {
            int deleted = vfat_delete_by_checksum(sector, 0, entry_idx, ck);
            if (deleted != 1) return -10;
        }
    }

    return 0;
}
