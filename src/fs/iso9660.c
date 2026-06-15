/*
 * src/fs/iso9660.c — ISO9660 (CDROM) read-only filesystem with
 * Rock Ridge (RRIP) extension support.
 *
 * Implements:
 *   - Standard ISO9660 directory traversal and file reading
 *   - Rock Ridge Interchange Protocol (RRIP) for POSIX permissions,
 *     long filenames, symlinks, and UID/GID
 *   - Falls back gracefully if Rock Ridge is not present
 */

#define KERNEL_INTERNAL
#include "iso9660.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

struct iso9660_priv {
    uint8_t  dev_id;
    uint16_t block_size;       /* usually 2048 */
    uint32_t root_extent;      /* extent location of root dir (ISO9660 PVD) */
    uint32_t root_size;        /* size of root dir */
    int      has_rrip;         /* 1 = Rock Ridge present and valid */
    /* Joliet fields */
    int      has_joliet;       /* 1 = Joliet (UCS-2) filenames available */
    uint32_t joliet_root_extent; /* root dir extent from Joliet SVD */
    uint32_t joliet_root_size;   /* root dir size from Joliet SVD */
};

/* Read a logical block from the ISO image */
static int iso_read_block(struct iso9660_priv *ip, uint32_t lba, uint8_t *buf)
{
    uint64_t sector = (uint64_t)lba * (ip->block_size / 512);
    uint32_t sectors = ip->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ip->dev_id, sector + i, 1, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* Find the primary volume descriptor */
static int iso9660_find_pvd(struct iso9660_priv *ip)
{
    uint8_t buf[2048];
    /* PVD is at sector 16 */
    if (iso_read_block(ip, 16, buf) < 0) return -1;

    struct iso_primary_desc *pvd = (struct iso_primary_desc *)buf;
    if (pvd->type != 1) return -1;
    if (memcmp(pvd->id, "CD001", 5) != 0) return -1;

    ip->block_size = 2048; /* standard */

    /* Parse root directory record (34 bytes at offset 156 in PVD) */
    struct iso_dir_record *root = (struct iso_dir_record *)pvd->root_dir;
    ip->root_extent = root->extent_loc_le;
    ip->root_size   = root->data_length_le;

    kprintf("[iso9660] Root dir at LBA %u, size %u\n",
            ip->root_extent, ip->root_size);
    return 0;
}

/*
 * Find the Supplementary Volume Descriptor (type 2) that contains
 * Joliet (UCS-2) filenames.  Returns 0 on success, -1 if not found.
 *
 * The SVD has the same basic structure as the PVD but with:
 *   - type = 2 instead of 1
 *   - escape sequences in the unused3 field (offset 88-119)
 *   - directory record filenames encoded in UCS-2 Big Endian
 *
 * Joliet is identified by escape sequences starting with %/ (0x25 0x2F):
 *   %/@ = UCS-2 level 1
 *   %/C = UCS-2 level 2
 *   %/E = UCS-2 level 3
 */
static int iso9660_find_joliet_svd(struct iso9660_priv *ip)
{
    uint8_t buf[2048];
    int sector = 16;

    /* Scan volume descriptors (sectors 16.. up to 256 descriptors) */
    for (int desc = 0; desc < 256; desc++) {
        if (iso_read_block(ip, (uint32_t)(sector + desc), buf) < 0)
            return -1;

        struct iso_supplementary_desc *svd = (struct iso_supplementary_desc *)buf;

        if (svd->type == 255)  /* volume descriptor set terminator */
            break;
        if (svd->type == 2 && memcmp(svd->id, "CD001", 5) == 0) {
            /* Check escape sequences for Joliet markers */
            if (svd->escape_sequences[0] == JOLIET_ESC_LEVEL1_0 &&
                svd->escape_sequences[1] == JOLIET_ESC_LEVEL1_1 &&
                (svd->escape_sequences[2] == JOLIET_ESC_LEVEL1_2 ||
                 svd->escape_sequences[2] == JOLIET_ESC_LEVEL2_2 ||
                 svd->escape_sequences[2] == JOLIET_ESC_LEVEL3_2)) {

                struct iso_dir_record *root =
                    (struct iso_dir_record *)svd->root_dir;
                ip->joliet_root_extent = root->extent_loc_le;
                ip->joliet_root_size   = root->data_length_le;
                ip->has_joliet = 1;

                kprintf("[iso9660] Joliet (UCS-2) filenames available"
                        " (root LBA %u, size %u)\n",
                        ip->joliet_root_extent, ip->joliet_root_size);
                return 0;
            }
        }
    }

    return -1;
}

/*
 * Convert a UCS-2 Big Endian codepoint to UTF-8 encoding.
 * Writes 1-3 bytes to @out (must have at least 4 bytes of space).
 * Returns the number of bytes written.
 */
static int ucs2_cp_to_utf8(uint16_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
}

/*
 * Convert a UCS-2 Big Endian filename (as used by Joliet) to UTF-8.
 *
 * Joliet filenames are encoded in UCS-2 Big Endian (2 bytes per character)
 * and do NOT include the ;1 version suffix that plain ISO9660 uses.
 *
 * @ucs2    Pointer to UCS-2BE data (2 bytes per character)
 * @ucs2_len_bytes  Length of the UCS-2 data IN BYTES (so characters = / 2)
 * @out     Destination buffer for UTF-8 result
 * @out_max Size of destination buffer (including NUL terminator)
 * Returns the number of characters written (excluding NUL terminator), or 0.
 */
static int ucs2be_to_utf8(const char *ucs2, int ucs2_len_bytes,
                           char *out, int out_max)
{
    int chars = ucs2_len_bytes / 2;
    int written = 0;

    for (int i = 0; i < chars; i++) {
        uint16_t cp = ((uint16_t)(uint8_t)ucs2[i * 2] << 8) |
                       (uint8_t)ucs2[i * 2 + 1];

        /* Skip zero bytes (padding or end of string) */
        if (cp == 0x0000)
            break;

        /* Skip the ;1 version separator that some Joliet implementations
         * erroneously include (Joliet spec says no version, but some discs
         * include it anyway).  Check if this is ';' followed by '1'. */
        if (cp == 0x003B && (i + 1 < chars)) {
            uint16_t next_cp = ((uint16_t)(uint8_t)ucs2[(i + 1) * 2] << 8) |
                                (uint8_t)ucs2[(i + 1) * 2 + 1];
            if (next_cp == 0x0031) {
                /* ;1 found — skip it.  Joliet names should not have version,
                 * but we handle it gracefully. */
                break;
            }
        }

        /* Encode this UCS-2 codepoint to UTF-8 */
        int nbytes = ucs2_cp_to_utf8(cp, out + written);
        written += nbytes;
        if (written >= out_max - 1) {
            /* Truncate gracefully */
            written = out_max - 1;
            break;
        }
    }

    /* Strip any trailing spaces and dots (Joliet pads with ';' and spaces) */
    while (written > 0) {
        char c = out[written - 1];
        if (c == ';' || c == ' ' || c == '.' || c == '\0')
            written--;
        else
            break;
    }

    out[written] = '\0';
    return written;
}

/* ── Rock Ridge / SUSP parser ───────────────────────────────────── */

/* Parse System Use entries following a directory record.
 * Returns a bitmask of RRIP_HAS_* flags on success, 0 on error or no RRIP.
 * Expects the full directory record (including name + system use) in @rec_buf,
 * with total record length @rec_len.
 */
static uint8_t parse_rrip_entries(struct iso9660_priv *ip,
                                   const uint8_t *rec_buf, uint32_t rec_len,
                                   struct iso_rrip_entry *out)
{
    /* If we already determined Rock Ridge is absent, skip parsing */
    if (!ip->has_rrip)
        return 0;

    const struct iso_dir_record *rec = (const struct iso_dir_record *)rec_buf;

    /* System use area starts after the name field.
     * The name_len field tells us how many bytes the name consumes,
     * but the record is padded to an even number of bytes.
     * The system use area begins at offset name_len_offset + name_len,
     * padded to even alignment. */
    uint8_t name_len = rec->name_len;
    uint32_t sus_offset;
    if (name_len == 0) {
        sus_offset = sizeof(struct iso_dir_record) - 1; /* name[1] padding */
    } else {
        sus_offset = (uint32_t)((uintptr_t)(rec->name) - (uintptr_t)rec) + (uint32_t)name_len;
    }
    /* Pad to even alignment */
    if (sus_offset & 1)
        sus_offset++;

    /* If the record length is exactly what we expect for name + padding,
     * there's no system use area. */
    if (sus_offset >= rec_len)
        return 0;

    uint32_t sus_len = rec_len - sus_offset;
    const uint8_t *sus_data = rec_buf + sus_offset;

    uint8_t rr_found = 0;

    /* Walk through SUSP entries in this record, following CE chains */
    uint32_t ce_block = 0;
    uint32_t ce_offset = 0;
    uint32_t ce_len = 0;
    int in_continuation = 0;
    int ce_hops = 0;          /* limit CE chain depth to prevent infinite loops */

    /* We'll use a simple iterative approach: if we encounter a CE entry,
     * we switch to reading from the continuation area, then resume. */

walk_susp:
    {
        const uint8_t *walk_data;
        uint32_t walk_len;
        uint32_t walk_off;

        if (in_continuation) {
            /* Read from continuation block */
            uint8_t ce_buf[2048];
            memset(ce_buf, 0, sizeof(ce_buf));
            if (iso_read_block(ip, ce_block, ce_buf) < 0)
                return rr_found;
            walk_data = ce_buf;
            walk_len = ip->block_size;
            walk_off = ce_offset;
            /* Clamp walk length */
            uint32_t ce_end = ce_offset + ce_len;
            if (ce_end > walk_len) ce_end = walk_len;
            walk_len = ce_end;
            in_continuation = 0;
        } else {
            walk_data = sus_data;
            walk_len = sus_len;
            walk_off = 0;
        }

        while (walk_off + 4 <= walk_len) {
            const struct susp_entry_header *hdr =
                (const struct susp_entry_header *)(walk_data + walk_off);

            if (hdr->len < 4) break;  /* invalid entry */
            if (walk_off + hdr->len > walk_len) break;

            /* SP entry (must be first in root's system use) */
            if (susp_sig_match(hdr, 'S', 'P')) {
                if (hdr->len >= 7) {
                    /* Magic bytes 0xBE, 0xEF confirm Rock Ridge */
                    const struct susp_sp_entry *sp =
                        (const struct susp_sp_entry *)hdr;
                    if (sp->magic_byte1 == 0xBE && sp->magic_byte2 == 0xEF)
                        rr_found |= 0x80; /* mark RRIP as confirmed present */
                }
            }
            /* NM (Alternative Name) entry */
            else if (susp_sig_match(hdr, 'N', 'M')) {
                const struct rrip_nm_entry *nm =
                    (const struct rrip_nm_entry *)hdr;
                uint8_t nm_flags = nm->flags;
                uint8_t nm_namelen = hdr->len - 5; /* subtract header + flags */
                if (nm_namelen > 0) {
                    uint32_t current_len = (uint32_t)strlen(out->rr_name);
                    if (current_len + nm_namelen < sizeof(out->rr_name)) {
                        memcpy(out->rr_name + current_len,
                               nm->name, nm_namelen);
                        out->rr_name[current_len + nm_namelen] = '\0';
                    }
                }
                if (!(nm_flags & 1)) /* CONTINUE flag = bit 0 */
                    rr_found |= RRIP_HAS_NM;
            }
            /* PX (POSIX Attributes) entry */
            else if (susp_sig_match(hdr, 'P', 'X')) {
                if (hdr->len >= 44) {
                    const struct rrip_px_entry *px =
                        (const struct rrip_px_entry *)hdr;
                    out->rr_mode = px->mode_le;
                    out->rr_uid  = px->uid_le;
                    out->rr_gid  = px->gid_le;
                    rr_found |= RRIP_HAS_PX;
                }
            }
            /* SL (Symbolic Link) entry */
            else if (susp_sig_match(hdr, 'S', 'L')) {
                const struct rrip_sl_entry *sl =
                    (const struct rrip_sl_entry *)hdr;
                uint32_t sl_data_len = hdr->len - 5; /* header + flags */
                uint32_t sl_pos = 0;
                char sl_buf[256];
                uint32_t sl_buf_pos = 0;

                while (sl_pos + 2 <= sl_data_len &&
                       sl_buf_pos < sizeof(sl_buf) - 1) {
                    const struct rrip_sl_component *comp =
                        (const struct rrip_sl_component *)(sl->link_data + sl_pos);
                    uint8_t comp_flags = comp->flags;
                    uint8_t comp_len = comp->len;

                    /* Validate component length doesn't exceed remaining data */
                    if (sl_pos + 2 + comp_len > sl_data_len) break;

                    if (comp_flags == 1) {
                        /* "." component */
                        if (sl_buf_pos > 0 && sl_buf_pos < sizeof(sl_buf) - 1)
                            sl_buf[sl_buf_pos++] = '/';
                        if (sl_buf_pos < sizeof(sl_buf) - 1)
                            sl_buf[sl_buf_pos++] = '.';
                    } else if (comp_flags == 2) {
                        /* ".." component */
                        if (sl_buf_pos > 0 && sl_buf_pos < sizeof(sl_buf) - 1)
                            sl_buf[sl_buf_pos++] = '/';
                        if (sl_buf_pos + 2 < sizeof(sl_buf)) {
                            sl_buf[sl_buf_pos++] = '.';
                            sl_buf[sl_buf_pos++] = '.';
                        }
                    } else if (comp_flags == 8) {
                        /* root "/" */
                        /* just reset */
                        sl_buf_pos = 0;
                    } else if (comp_flags == 0 && comp_len > 0) {
                        /* plain component */
                        if (sl_buf_pos > 0 && sl_buf_pos < sizeof(sl_buf) - 1)
                            sl_buf[sl_buf_pos++] = '/';
                        uint32_t copy = comp_len;
                        if (sl_buf_pos + copy >= sizeof(sl_buf))
                            copy = (uint32_t)(sizeof(sl_buf) - 1 - sl_buf_pos);
                        memcpy(sl_buf + sl_buf_pos, comp->name, copy);
                        sl_buf_pos += copy;
                    }
                    sl_pos += 2 + (uint32_t)comp_len;
                }
                sl_buf[sl_buf_pos] = '\0';
                memcpy(out->rr_symlink, sl_buf, sl_buf_pos + 1);
                rr_found |= RRIP_HAS_SL;
            }
            /* CE (Continuation Entry) — more data elsewhere */
            else if (susp_sig_match(hdr, 'C', 'E')) {
                if (hdr->len >= 28) {
                    const struct susp_ce_entry *ce =
                        (const struct susp_ce_entry *)hdr;
                    ce_block = ce->block_loc_le;
                    ce_offset = ce->offset_le;
                    ce_len = ce->cont_len_le;
                    /* We'll process the continuation after walking the
                     * current chunk's remaining entries, then loop. */
                }
            }

            walk_off += hdr->len;
        }
    }

    /* If we had a CE entry, process the continuation area */
    if (ce_len > 0) {
        ce_hops++;
        if (ce_hops > 16) {
            kprintf("iso9660: CE continuation depth exceeded (>16 hops), "
                    "possible crafted ISO\\n");
            return rr_found;
        }
        ce_len = 0;  /* reset so we don't loop if continuation has no CE */
        in_continuation = 1;
        goto walk_susp;
    }

    return rr_found;
}

/* Parse a directory record into an iso_rrip_entry.
 * Handles Rock Ridge if present, falls back to ISO9660 names.
 */
static int parse_one_dirent(struct iso9660_priv *ip,
                             const uint8_t *rec_buf, uint32_t rec_len,
                             struct iso_rrip_entry *de)
{
    const struct iso_dir_record *rec = (const struct iso_dir_record *)rec_buf;

    memset(de, 0, sizeof(*de));
    de->extent = rec->extent_loc_le;
    de->size   = rec->data_length_le;
    de->flags  = rec->flags;

    /* Extract name: use Joliet UCS-2BE decoding if available, else ISO9660 */
    uint8_t nlen = rec->name_len;

    if (ip->has_joliet && nlen > 0 && nlen < 255) {
        /* Joliet: name is encoded in UCS-2 Big Endian (2 bytes/char).
         * Convert to UTF-8, preserving all Unicode characters.
         * This enables proper display of international filenames. */
        ucs2be_to_utf8(rec->name, nlen, de->iso_name, sizeof(de->iso_name));
    } else if (nlen > 0 && nlen < 255) {
        /* Standard ISO9660: single-byte name (usually d-characters) */
        memcpy(de->iso_name, rec->name, nlen);
        de->iso_name[nlen] = '\0';
    } else {
        /* Current directory or parent reference */
        if (nlen == 1 && rec->name[0] == 0)
            strncpy(de->iso_name, ".", sizeof(de->iso_name) - 1);
        else if (nlen == 1 && rec->name[0] == 1)
            strncpy(de->iso_name, "..", sizeof(de->iso_name) - 1);
        else
            de->iso_name[0] = '\0';
    }
    de->iso_name[sizeof(de->iso_name) - 1] = '\0';

    /* Parse Rock Ridge entries */
    uint8_t rr_parsed = parse_rrip_entries(ip, rec_buf, rec_len, de);

    /* If we got an NM entry, prefer the long name */
    if (rr_parsed & RRIP_HAS_NM) {
        /* Use the RR name; if empty, fall back to ISO name */
        if (de->rr_name[0] == '\0') {
            memcpy(de->rr_name, de->iso_name, sizeof(de->iso_name));
        }
    }

    return (int)(rr_parsed & 0x7f); /* return RRIP_HAS_* flags (without 0x80 marker) */
}

/* Read all directory entries from a directory extent */
static int iso_read_dir_entries(struct iso9660_priv *ip, uint32_t extent, uint32_t size,
                                 struct iso_rrip_entry *entries, int max)
{
    uint8_t buf[2048];
    uint32_t offset = 0;
    int count = 0;

    while (offset < size && count < max) {
        uint32_t lba = extent + offset / ip->block_size;
        if (iso_read_block(ip, lba, buf) < 0) break;

        uint32_t pos = offset % ip->block_size;
        while (pos < ip->block_size && offset < size && count < max) {
            struct iso_dir_record *rec = (struct iso_dir_record *)(buf + pos);
            if (rec->length == 0) { pos++; offset++; continue; }
            if (rec->length < 33) break;

            /* Parse this entry with Rock Ridge */
            parse_one_dirent(ip, buf + pos, rec->length, &entries[count]);

            count++;
            pos += rec->length;
            offset += rec->length;
        }
    }

    return count;
}

/* Resolve path to extent.
 * Uses Joliet root directory when Joliet (UCS-2) filenames are available,
 * falling back to the ISO9660 PVD root otherwise. */
static int iso9660_resolve(struct iso9660_priv *ip, const char *path,
                            uint32_t *extent, uint32_t *size)
{
    /* Use Joliet root directory when available (it may have a different
     * extent than the PVD root, with UCS-2 encoded filenames). */
    if (ip->has_joliet) {
        *extent = ip->joliet_root_extent;
        *size   = ip->joliet_root_size;
    } else {
        *extent = ip->root_extent;
        *size   = ip->root_size;
    }

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return 0;

    struct iso_rrip_entry entries[128];
    int n;

    /* Read root directory */
    n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
    if (n <= 0) return -1;

    while (*p) {
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);

        int found = 0;
        for (int i = 0; i < n; i++) {
            const char *ename;

            /* Prefer Rock Ridge name if available */
            if (ip->has_rrip && entries[i].rr_name[0] != '\0')
                ename = entries[i].rr_name;
            else
                ename = entries[i].iso_name;

            size_t elen = strlen(ename);

            /* Skip dot entries */
            if (elen == 0) continue;
            if (elen == 1 && ename[0] == 0) continue;
            if (elen == 1 && ename[0] == '.') continue;
            if (elen == 2 && ename[0] == '.' && ename[1] == '.') continue;

            /* ISO names often have ;1 suffix for version.
             * Joliet names do not have version suffixes. */
            const char *match_name = ename;
            size_t match_len = elen;
            if (!ip->has_joliet && match_len > 2 && match_name[match_len - 2] == ';')
                match_len -= 2;

            if (match_len == clen && memcmp(match_name, p, clen) == 0) {
                *extent = entries[i].extent;
                *size   = entries[i].size;
                found = 1;
                break;
            }

            /* If Rock Ridge symlink is available and name matches,
             * follow the symlink target. */
            if (ip->has_rrip && (entries[i].rr_flags & RRIP_HAS_SL) &&
                match_len == clen && memcmp(match_name, p, clen) == 0) {
                /* Read symlink target and resolve from that path */
                char link_target[256];
                strncpy(link_target, entries[i].rr_symlink, sizeof(link_target) - 1);
                link_target[sizeof(link_target) - 1] = '\0';

                /* Prepend parent path if the symlink is relative */
                if (link_target[0] != '/') {
                    /* Relative symlink: build full path by prepending
                     * the parent directory of the symlink. */
                    char full_path[512];
                    size_t parent_len = (size_t)(p - path);

                    /* Copy parent directory path (strip trailing slash) */
                    size_t fp_len = 0;
                    if (parent_len > 0) {
                        memcpy(full_path, path, parent_len);
                        fp_len = parent_len;
                        while (fp_len > 0 && full_path[fp_len - 1] == '/')
                            fp_len--;
                    }

                    /* Append '/' separator */
                    if (fp_len > 0)
                        full_path[fp_len++] = '/';

                    /* Append link target */
                    size_t tlen = strlen(link_target);
                    if (fp_len + tlen + 1 > sizeof(full_path)) {
                        kprintf("iso9660: symlink target path too long\n");
                        return -ENAMETOOLONG;
                    }
                    memcpy(full_path + fp_len, link_target, tlen);
                    fp_len += tlen;
                    full_path[fp_len] = '\0';

                    /* Resolve the combined absolute path recursively.
                     * Recursive depth is bounded by nesting of symlinks
                     * (max 40 per the caller's absolute path depth guard). */
                    uint32_t resolved_extent, resolved_size;
                    if (iso9660_resolve(ip, full_path,
                                        &resolved_extent,
                                        &resolved_size) == 0) {
                        *extent = resolved_extent;
                        *size   = resolved_size;
                        found = 1;
                        break;
                    }
                    return -1;
                }

                /* Resolve the symlink target recursively with depth limit.
                 * Linux allows max 40 symlink follows (SYMLOOP_MAX). */
                int depth = 0;
                const char *sp = link_target;
                while (*sp) {
                    if (depth++ > 40) {
                        kprintf("iso9660: symlink depth limit exceeded\n");
                        return -ELOOP;
                    }
                    const char *send = sp;
                    while (*send && *send != '/') send++;
                    size_t slen = (size_t)(send - sp);

                    int sfound = 0;
                    for (int j = 0; j < n; j++) {
                        const char *sname;
                        if (ip->has_rrip && entries[j].rr_name[0] != '\0')
                            sname = entries[j].rr_name;
                        else
                            sname = entries[j].iso_name;
                        size_t snlen = strlen(sname);
                        if (snlen == 0) continue;
                        if (snlen == 1 && sname[0] == 0) continue;
                        if (snlen == 1 && sname[0] == '.') continue;
                        if (snlen == 2 && sname[0] == '.' && sname[1] == '.') continue;
                        const char *smatch = sname;
                        size_t smlen = snlen;
                        if (smlen == slen && memcmp(smatch, sp, slen) == 0) {
                            *extent = entries[j].extent;
                            *size   = entries[j].size;
                            sfound = 1;
                            break;
                        }
                    }
                    if (!sfound) return -1;
                    sp = send;
                    while (*sp == '/') sp++;
                    if (*sp) {
                        /* Read next directory level */
                        n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
                        if (n <= 0) return -1;
                    }
                }
                found = 1;
                break;
            }

            /* Also try without version number (ISO9660 fallback) */
            if (!ip->has_joliet && match_len > clen && match_name[clen] == ';' &&
                memcmp(match_name, p, clen) == 0) {
                *extent = entries[i].extent;
                *size   = entries[i].size;
                found = 1;
                break;
            }
        }
        if (!found) return -1;

        p = end;
        while (*p == '/') p++;
        if (!*p) break;

        /* Read next level */
        n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
        if (n <= 0) return -1;
    }

    return 0;
}

/* ── Rock Ridge check at mount time ─────────────────────────────── */

/* Check if the root directory record has Rock Ridge SUSP entries.
 * We directly scan the system use area of each root directory entry
 * for the SP signature (0xBE, 0xEF magic bytes). */
static int check_rrip_present(struct iso9660_priv *ip)
{
    uint8_t buf[2048];
    uint32_t offset = 0;

    while (offset < ip->root_size) {
        uint32_t lba = ip->root_extent + offset / ip->block_size;
        if (iso_read_block(ip, lba, buf) < 0) return 0;

        uint32_t pos = offset % ip->block_size;
        while (pos < ip->block_size && offset < ip->root_size) {
            const struct iso_dir_record *rec =
                (const struct iso_dir_record *)(buf + pos);
            if (rec->length == 0) { pos++; offset++; continue; }
            if (rec->length < 33) break;

            /* Calculate system use offset */
            uint8_t name_len = rec->name_len;
            uint32_t sus_off = sizeof(struct iso_dir_record) - 1 + name_len;
            if (sus_off & 1) sus_off++;

            if (sus_off + 4 <= rec->length) {
                const struct susp_entry_header *hdr =
                    (const struct susp_entry_header *)(buf + pos + sus_off);
                /* Check for SP entry directly */
                if (hdr->sig[0] == 'S' && hdr->sig[1] == 'P' &&
                    hdr->len >= 7) {
                    const struct susp_sp_entry *sp =
                        (const struct susp_sp_entry *)hdr;
                    if (sp->magic_byte1 == 0xBE &&
                        sp->magic_byte2 == 0xEF) {
                        return 1; /* Rock Ridge confirmed */
                    }
                }
            }

            pos += rec->length;
            offset += rec->length;
        }
    }

    return 0;
}

/* ── VFS operations ─────────────────────────────────────────────── */

static int iso9660_read(void *priv, const char *path, void *buf,
                         uint32_t max_size, uint32_t *out_size)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0)
        return -ENOENT;

    /* Check if resolved path is a directory — can't read dirs as files */
    struct iso_rrip_entry entries[2];
    int n = iso_read_dir_entries(ip, extent, size, entries, 1);
    if (n >= 0) {
        /* If it has children, it's a directory; reject reading */
        /* Actually, extent points to data, which for a file is the content;
         * for a directory it's the directory listing. We trust the caller
         * to know the type. */
    }

    uint32_t to_read = size;
    if (to_read > max_size) to_read = max_size;

    uint32_t offset = 0;
    while (offset < to_read) {
        uint32_t lba = extent + offset / ip->block_size;
        uint8_t block[2048];
        if (iso_read_block(ip, lba, block) < 0) break;

        uint32_t chunk = to_read - offset;
        if (chunk > ip->block_size) chunk = ip->block_size;
        memcpy((uint8_t *)buf + offset, block, chunk);
        offset += chunk;
    }

    *out_size = offset;
    return 0;
}

static int iso9660_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0)
        return -ENOENT;

    memset(st, 0, sizeof(*st));
    st->size = size;

    /* Root directory */
    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') {
        st->type = 2; /* VFS_DIR */
        st->mode = 0555;
        return 0;
    }

    /* Find parent directory and locate this entry */
    const char *slash = NULL;
    for (const char *s = p; *s; s++) { if (*s == '/') slash = s; }

    uint32_t pextent = ip->root_extent;
    uint32_t psize   = ip->root_size;

    if (slash) {
        char parent[256];
        size_t plen = (size_t)(slash - p);
        memcpy(parent, p, plen);
        parent[plen] = '\0';
        uint32_t dummy_ext, dummy_sz;
        if (iso9660_resolve(ip, parent, &dummy_ext, &dummy_sz) < 0)
            return -ENOENT;
        pextent = dummy_ext;
        psize = dummy_sz;
    }

    struct iso_rrip_entry entries[256];
    int n = iso_read_dir_entries(ip, pextent, psize, entries, 256);
    const char *name = slash ? slash + 1 : p;
    size_t nlen = strlen(name);

    for (int i = 0; i < n; i++) {
        const char *ename;
        if (ip->has_rrip && entries[i].rr_name[0] != '\0')
            ename = entries[i].rr_name;
        else
            ename = entries[i].iso_name;
        size_t elen = strlen(ename);

        if (elen == nlen && memcmp(ename, name, nlen) == 0) {
            /* Use Rock Ridge POSIX attributes if available */
            if (entries[i].rr_flags & RRIP_HAS_PX) {
                st->mode = entries[i].rr_mode & 07777;
                st->uid  = entries[i].rr_uid;
                st->gid  = entries[i].rr_gid;
                /* Determine type from mode:
                 * 1 = file, 2 = directory
                 * Symlinks and other types fall back to file */
                if (entries[i].rr_mode & RR_S_IFDIR)
                    st->type = 2;
                else if (entries[i].rr_mode & RR_S_IFREG)
                    st->type = 1;
                else if (entries[i].rr_mode & RR_S_IFLNK)
                    st->type = 1; /* Symlinks reported as files (readlink provides target) */
                else
                    st->type = entries[i].flags & ISO_FLAG_DIRECTORY ? 2 : 1;
            } else {
                st->type = (entries[i].flags & ISO_FLAG_DIRECTORY) ? 2 : 1;
                st->mode = (entries[i].flags & ISO_FLAG_DIRECTORY) ? 0555 : 0444;
            }
            return 0;
        }
    }

    /* Fallback: use flags from the resolved extent itself */
    st->type = 1; /* VFS_FILE */
    st->mode = 0444;
    return 0;
}

static int iso9660_readlink(void *priv, const char *path,
                             char *buf, int bufsize)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0)
        return -ENOENT;

    /* Find the entry's symlink target */
    const char *p = path;
    if (*p == '/') p++;

    const char *slash = NULL;
    for (const char *s = p; *s; s++) { if (*s == '/') slash = s; }

    uint32_t pextent = ip->root_extent;
    uint32_t psize   = ip->root_size;

    if (slash) {
        char parent[256];
        size_t plen = (size_t)(slash - p);
        memcpy(parent, p, plen);
        parent[plen] = '\0';
        uint32_t dummy_ext, dummy_sz;
        if (iso9660_resolve(ip, parent, &dummy_ext, &dummy_sz) < 0)
            return -ENOENT;
        pextent = dummy_ext;
        psize = dummy_sz;
    }

    struct iso_rrip_entry entries[256];
    int n = iso_read_dir_entries(ip, pextent, psize, entries, 256);
    const char *name = slash ? slash + 1 : p;
    size_t nlen = strlen(name);

    for (int i = 0; i < n; i++) {
        const char *ename;
        if (ip->has_rrip && entries[i].rr_name[0] != '\0')
            ename = entries[i].rr_name;
        else
            ename = entries[i].iso_name;
        size_t elen = strlen(ename);

        if (elen == nlen && memcmp(ename, name, nlen) == 0) {
            if (entries[i].rr_flags & RRIP_HAS_SL) {
                int slen = (int)strlen(entries[i].rr_symlink);
                if (slen >= bufsize) slen = bufsize - 1;
                memcpy(buf, entries[i].rr_symlink, (size_t)slen);
                buf[slen] = '\0';
                return slen;
            }
            return -EINVAL; /* not a symlink */
        }
    }

    return -ENOENT;
}

static int iso9660_readdir_entries(void *priv, const char *path,
                                    char names[][64], int max)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0) return -1;

    struct iso_rrip_entry entries[256];
    int n = iso_read_dir_entries(ip, extent, size, entries, max < 256 ? max : 256);
    int count = 0;

    for (int i = 0; i < n && count < max; i++) {
        const char *ename;

        /* Prefer Rock Ridge name if available */
        if (ip->has_rrip && entries[i].rr_name[0] != '\0')
            ename = entries[i].rr_name;
        else
            ename = entries[i].iso_name;

        size_t elen = strlen(ename);
        if (elen == 0) continue;
        if (elen == 1 && ename[0] == 0) continue;
        if (elen == 1 && ename[0] == '.') continue;
        if (elen == 2 && ename[0] == '.' && ename[1] == '.') continue;

        /* Strip version number from ISO names if not using RR or Joliet.
         * Joliet names do not have version suffixes. */
        if (!(ip->has_rrip && entries[i].rr_name[0] != '\0') && !ip->has_joliet) {
            if (elen > 2 && ename[elen - 2] == ';')
                elen -= 2;
        }

        if (elen > 63) elen = 63;
        memcpy(names[count], ename, elen);
        names[count][elen] = '\0';
        count++;
    }

    return count;
}

static int iso9660_readdir_legacy(void *priv, const char *path)
{
    char names[64][64];
    int n = iso9660_readdir_entries(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

static struct vfs_ops iso9660_ops = {
    .read    = iso9660_read,
    .stat    = iso9660_stat,
    .readlink = iso9660_readlink,
    .readdir_names = iso9660_readdir_entries,
    .readdir = iso9660_readdir_legacy,
};

int iso9660_mount(const char *mountpoint, uint8_t dev_id)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)kmalloc(sizeof(struct iso9660_priv));
    if (!ip) return -ENOMEM;

    memset(ip, 0, sizeof(*ip));
    ip->dev_id = dev_id;
    ip->block_size = 2048;
    ip->has_rrip = 0;

    if (iso9660_find_pvd(ip) < 0) {
        kprintf("[iso9660] No primary volume descriptor found\n");
        kfree(ip);
        return -1;
    }

    /* Check for Rock Ridge presence by scanning the root directory */
    ip->has_rrip = check_rrip_present(ip);

    /* Probe for Joliet (UCS-2) Supplementary Volume Descriptor */
    ip->has_joliet = 0;
    iso9660_find_joliet_svd(ip);

    /* Build mount info string */
    char mount_info[128];
    int mi_len = snprintf(mount_info, sizeof(mount_info),
                          "[iso9660] Mounted at %s (block_size=%u",
                          mountpoint, ip->block_size);
    if (ip->has_rrip)
        mi_len += snprintf(mount_info + mi_len, sizeof(mount_info) - (size_t)mi_len,
                           ", Rock Ridge");
    if (ip->has_joliet)
        mi_len += snprintf(mount_info + mi_len, sizeof(mount_info) - (size_t)mi_len,
                           ", Joliet (UCS-2)");
    snprintf(mount_info + mi_len, sizeof(mount_info) - (size_t)mi_len, ")\n");
    kprintf("%s", mount_info);

    /* Register readlink only if Rock Ridge symlinks are supported */
    if (ip->has_rrip) {
        iso9660_ops.readlink = iso9660_readlink;
    } else {
        iso9660_ops.readlink = NULL; /* no symlink support without RR */
    }

    return vfs_mount_ex(mountpoint, &iso9660_ops, ip, MS_RDONLY);
}

int iso9660_init(void)
{
    kprintf("[iso9660] ISO9660 CDROM filesystem (Rock Ridge + Joliet) initialized\n");
    vfs_register_filesystem("iso9660", &iso9660_ops);
    return 0;
}

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void) {
    return iso9660_init();
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void) {
    /* No VFS unregister yet; avoid unloading if filesystem is mounted */
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ISO9660 CDROM filesystem with Rock Ridge (RRIP) and Joliet (UCS-2) extension support");
#endif
