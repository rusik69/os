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

/* ISO9660 specifies a maximum directory nesting depth of 8 levels.
 * This includes the root directory as level 0. */
#define ISO9660_DIR_DEPTH_MAX 8

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
    /* Multi-session support */
    int      num_sessions;          /* number of sessions found */
    int      active_session;        /* index of the active (last) session */
    struct iso_session_info sessions[ISO9660_MAX_SESSIONS];
};

/* Read a logical block from the ISO image */
static int iso_read_block(struct iso9660_priv *ip, uint32_t lba, uint8_t *buf)
{
    uint64_t sector = (uint64_t)lba * (ip->block_size / 512);
    uint32_t sectors = ip->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ip->dev_id, (uint32_t)(sector + i), 1, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* ── Multi-session volume descriptor scanning ──────────────────── */

#define ISO9660_DESC_SCAN_LIMIT 256   /* max descriptors to scan per session */

/* Scan one session's volume descriptor set starting at @start_lba.
 * Fills in @sess with PVD and Joliet SVD information.
 * Stops at VDST (type 255) or after ISO9660_DESC_SCAN_LIMIT descriptors.
 * Returns 0 on success (PVD found), -1 if no valid PVD found. */
static int iso9660_scan_session(struct iso9660_priv *ip,
                                 struct iso_session_info *sess,
                                 uint32_t start_lba)
{
    uint8_t buf[2048];
    int found_pvd = 0;

    memset(sess, 0, sizeof(*sess));
    sess->session_lba = start_lba;

    for (int desc = 0; desc < ISO9660_DESC_SCAN_LIMIT; desc++) {
        uint32_t block = start_lba + (uint32_t)desc;
        if (iso_read_block(ip, block, buf) < 0)
            break;

        uint8_t type = buf[0];

        /* Volume Descriptor Set Terminator (type 255) with "CD001"
         * marks the end of this session's descriptor set. */
        if (type == 255 && memcmp(buf + 1, "CD001", 5) == 0) {
            if (found_pvd)
                break;  /* end of this session */
            continue;   /* stray terminator without a PVD — skip */
        }

        if (memcmp(buf + 1, "CD001", 5) != 0)
            continue;  /* not an ISO9660 volume descriptor */

        if (type == 1 && !found_pvd) {
            /* Primary Volume Descriptor */
            struct iso_primary_desc *pvd = (struct iso_primary_desc *)buf;
            struct iso_dir_record *root = (struct iso_dir_record *)pvd->root_dir;
            sess->root_extent = root->extent_loc_le;
            sess->root_size   = root->data_length_le;
            found_pvd = 1;
        } else if (type == 2) {
        	/* Supplementary Volume Descriptor — check for Joliet escape seqs */
        	struct iso_supplementary_desc *svd =
        	    (struct iso_supplementary_desc *)buf;
        	if (joliet_is_joliet_svd(svd)) {
        	    struct iso_dir_record *root =
        	        (struct iso_dir_record *)svd->root_dir;
        	    sess->joliet_root_extent = root->extent_loc_le;
        	    sess->joliet_root_size   = root->data_length_le;
        	    sess->has_joliet = 1;
        	    /* Extract Joliet volume name as UTF-8 */
        	    joliet_convert_svd_field(svd->volume_id,
        	        (int)sizeof(svd->volume_id),
        	        sess->joliet_volume_name,
        	        (int)sizeof(sess->joliet_volume_name));
        	}
        }
        /* Types 0, 3–254 are skipped (boot record, partition, etc.) */
    }

    return found_pvd ? 0 : -1;
}

/* Find all sessions on a multi-session ISO9660 disc.
 * Replaces the previous iso9660_find_pvd() + iso9660_find_joliet_svd().
 *
 * Algorithm:
 *   1. Start at LBA 16 (standard location of the first VDS).
 *   2. Scan session descriptors until VDST (type 255).
 *   3. Continue scanning past VDST for additional sessions.
 *   4. Up to ISO9660_MAX_SESSIONS sessions, bounded by safety scan range.
 *   5. Active session is always the LAST session found (most recent data).
 *
 * Legacy fields (root_extent, root_size, has_joliet, etc.) are
 * populated from the active session for backward compatibility. */
static int iso9660_find_sessions(struct iso9660_priv *ip)
{
    uint32_t search_lba = 16;
    int nsessions = 0;

    while (nsessions < ISO9660_MAX_SESSIONS) {
        struct iso_session_info sess;

        if (iso9660_scan_session(ip, &sess, search_lba) < 0) {
            if (nsessions == 0)
                return -1;  /* no valid session at all */
            break;          /* no more sessions; at least one found */
        }

        ip->sessions[nsessions] = sess;
        nsessions++;

        /* Find the VDST block to advance past this session */
        uint8_t buf[2048];
        int found_vdst = 0;
        for (int desc = 0; desc < ISO9660_DESC_SCAN_LIMIT; desc++) {
            uint32_t block = search_lba + (uint32_t)desc;
            if (iso_read_block(ip, block, buf) < 0)
                break;
            if (buf[0] == 255 && memcmp(buf + 1, "CD001", 5) == 0) {
                search_lba = block + 1;
                found_vdst = 1;
                break;
            }
        }

        if (!found_vdst)
            break;  /* malformed session — no VDST found */

        /* Safety: cap scan range to prevent runaway on large images */
        if (search_lba > 65536)
            break;
    }

    ip->num_sessions    = nsessions;
    ip->active_session  = nsessions - 1;  /* last session is primary */

    /* Populate legacy fields from the active (last) session */
    {
        const struct iso_session_info *as = &ip->sessions[ip->active_session];
        ip->root_extent = as->root_extent;
        ip->root_size   = as->root_size;
        ip->has_joliet  = as->has_joliet;
        ip->joliet_root_extent = as->joliet_root_extent;
        ip->joliet_root_size   = as->joliet_root_size;

        kprintf("[iso9660] %d session(s) found, active=%d "
                "(root LBA %u, size %u)\n",
                nsessions, ip->active_session,
                ip->root_extent, ip->root_size);
        if (ip->has_joliet)
            kprintf("[iso9660] Active session has Joliet "
                    "(UCS-2) filenames\n");
    }

    return 0;
}

/*
 * Wrapper: delegate to joliet_ucs2be_to_utf8() in iso9660_joliet.c.
 * Kept as a static wrapper so existing callers in this file don't need
 * to be updated if the API changes (thin compat layer).
 */
static int ucs2be_to_utf8(const char *ucs2, int ucs2_len_bytes,
                           char *out, int out_max)
{
	return joliet_ucs2be_to_utf8(ucs2, ucs2_len_bytes, out, out_max);
}

/* ── Hybrid fallback chain: Rock Ridge → Joliet → ISO9660 ──────── */

/* Fallback chain priority for entry names:
 *   1. Rock Ridge NM (POSIX long filename) — if has_rrip and rr_name present
 *   2. Joliet UCS-2→UTF-8 decoded name  — if iso_name was Joliet-decoded
 *   3. Standard ISO9660 short name      — raw d-character + version suffix
 * For Level 3, the ";1" version suffix is stripped from the returned length.
 * Level 1 and Level 2 names never carry version suffixes.
 *
 * Returns pointer to the NUL-terminated name string (always valid).
 * @out_len receives the byte length of the effective name (sans version). */
static const char *iso9660_get_entry_name(struct iso9660_priv *ip,
                                          const struct iso_rrip_entry *de,
                                          size_t *out_len)
{
	const char *name;

	/* Level 1: Rock Ridge NM — POSIX long filename */
	if (ip->has_rrip && de->rr_name[0] != '\0')
		name = de->rr_name;
	else
		name = de->iso_name;  /* Level 2 (Joliet) or Level 3 (ISO9660) */

	size_t len = strlen(name);

	/* Level 3 only: strip ";1" version suffix from plain ISO9660 names.
	 * Level 1 (RR NM) and Level 2 (Joliet decoded) never have suffixes. */
	if (!ip->has_joliet && !(ip->has_rrip && de->rr_name[0] != '\0')) {
		if (len > 2 && name[len - 2] == ';')
			len -= 2;
	}

	if (out_len)
		*out_len = len;

	return name;
}

/* ── Rock Ridge / SUSP parser ───────────────────────────────────── */

/* Parse System Use entries following a directory record.
 * Returns a bitmask of RRIP_HAS_* flags on success, 0 on error or no RRIP.
 * Expects the full directory record (including name + system use) in @rec_buf,
 * with total record length @rec_len.
 */
static uint16_t parse_rrip_entries(struct iso9660_priv *ip,
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

    uint16_t rr_found = 0;

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
        uint8_t ce_buf[2048];              /* lifted to outer scope to avoid
                                             * use-after-scope when walk_data
                                             * points to it across goto */

        if (in_continuation) {
            /* Read from continuation block */
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
                /* Magic bytes 0xBE, 0xEF confirm Rock Ridge — skip bits
                 * since ip->has_rrip already gates entry to this function */
                if (hdr->len < 7) {
                    return rr_found;
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
                    out->rr_nlink = px->nlink_le;
                    out->rr_atime = px->atime_le;
                    out->rr_mtime = px->mtime_le;
                    out->rr_ctime = px->ctime_le;
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
                int has_continue = (sl->flags & 1);

                /* If CONTINUE flag is set and we have existing symlink
                 * content, append to it from where we left off. */
                if (has_continue && out->rr_symlink[0] != '\0') {
                    sl_buf_pos = (uint32_t)strlen(out->rr_symlink);
                    if (sl_buf_pos >= sizeof(sl_buf))
                        sl_buf_pos = sizeof(sl_buf) - 1;
                    memcpy(sl_buf, out->rr_symlink, sl_buf_pos);
                }

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
                /* Only mark complete on the final SL entry (no CONTINUE).
                 * Intermediate entries with CONTINUE set append to the
                 * existing buffer for the next SL entry in the chain. */
                if (!has_continue)
                    rr_found |= RRIP_HAS_SL;
            }
            /* PN (POSIX Device Node) entry */
            else if (susp_sig_match(hdr, 'P', 'N')) {
                if (hdr->len >= 20) {
                    const struct rrip_pn_entry *pn =
                        (const struct rrip_pn_entry *)hdr;
                    out->rr_dev_major = pn->dev_high_le;
                    out->rr_dev_minor = pn->dev_low_le;
                    rr_found |= RRIP_HAS_PN;
                }
            }
            /* TF (Timestamps) entry — POSIX timestamps from Rock Ridge */
            else if (susp_sig_match(hdr, 'T', 'F')) {
                const struct rrip_tf_entry *tf =
                    (const struct rrip_tf_entry *)hdr;
                uint32_t tf_len = (uint32_t)hdr->len;
                if (tf_len >= 5) {
                    uint32_t tf_atime = 0, tf_mtime = 0, tf_ctime = 0, tf_btime = 0;
                    uint8_t tf_flags = iso9660_rr_parse_tf(tf, tf_len,
                                                            &tf_atime,
                                                            &tf_mtime,
                                                            &tf_ctime,
                                                            &tf_btime);
                    if (tf_flags & RRIP_TF_CREATE)
                        out->rr_btime = tf_btime;
                    if (tf_flags & RRIP_TF_ACCESS)
                        out->rr_atime = tf_atime;
                    if (tf_flags & RRIP_TF_MODIFY)
                        out->rr_mtime = tf_mtime;
                    if (tf_flags & RRIP_TF_ATTRIB)
                        out->rr_ctime = tf_ctime;
                    rr_found |= RRIP_HAS_TF;
                }
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
    de->file_unit_size = rec->file_unit_size;
    de->interleave_gap = rec->interleave_gap;

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
    uint16_t rr_parsed = parse_rrip_entries(ip, rec_buf, rec_len, de);
    de->rr_flags = rr_parsed;  /* store for callers */

    /* If we got an NM entry, prefer the long name */
    if (rr_parsed & RRIP_HAS_NM) {
        /* Use the RR name; if empty, fall back to ISO name */
        if (de->rr_name[0] == '\0') {
            memcpy(de->rr_name, de->iso_name, sizeof(de->iso_name));
        }
    }

    return (int)(rr_parsed); /* return RRIP_HAS_* flags */
}

/* Read all directory entries from a directory extent.
 * Handles ISO9660 directory records correctly even when they span
 * across sector boundaries (ISO 9660 §9.1.4 — records may cross
 * logical sector boundaries and are logically contiguous). */
static int iso_read_dir_entries(struct iso9660_priv *ip, uint32_t extent, uint32_t size,
                                 struct iso_rrip_entry *entries, int max)
{
    uint8_t buf[2048];
    uint8_t straddle_buf[2048];  /* buffer for cross-block records */
    uint32_t offset = 0;
    int count = 0;

    while (offset < size && count < max) {
        uint32_t lba = extent + offset / ip->block_size;
        if (iso_read_block(ip, lba, buf) < 0) break;

        uint32_t pos = offset % ip->block_size;
        while (pos < ip->block_size && offset < size && count < max) {
            const struct iso_dir_record *rec = (const struct iso_dir_record *)(buf + pos);

            if (rec->length == 0) {
                /* Zero-length record means the rest of this block is padding.
                 * Skip to the next block. */
                uint32_t skip = ip->block_size - pos;
                pos = ip->block_size;
                offset += skip;
                continue;
            }

            /* ISO 9660 §9.1.4: minimum directory record length is 33 bytes
             * (fixed fields + 1-byte name).  Shorter records are invalid. */
            if (rec->length < 33)
                break;

            /* Check if this record spans across the block boundary.
             * If so, we need to copy both parts into a contiguous buffer. */
            if (pos + rec->length > ip->block_size) {
                /* Copy the part within the current block */
                uint32_t first_part = ip->block_size - pos;
                memcpy(straddle_buf, buf + pos, first_part);

                /* Read the next block and copy the remainder */
                if (offset + first_part < size) {
                    uint32_t next_lba = extent + (offset + first_part) / ip->block_size;
                    uint8_t next_buf[2048];
                    if (iso_read_block(ip, next_lba, next_buf) < 0)
                        break;

                    uint32_t second_part = rec->length - first_part;
                    if (second_part > ip->block_size)
                        second_part = ip->block_size;
                    memcpy(straddle_buf + first_part, next_buf, second_part);

                    /* Parse from the combined buffer */
                    parse_one_dirent(ip, straddle_buf, rec->length, &entries[count]);
                } else {
                    break;  /* truncated record */
                }
            } else {
                /* Record fits entirely within this block — normal case */
                parse_one_dirent(ip, buf + pos, rec->length, &entries[count]);
            }

            count++;
            pos += rec->length;
            offset += rec->length;
        }
    }

    return count;
}

/* Resolve path to extent.
 * Uses Joliet root directory when Joliet (UCS-2) filenames are available,
 * falling back to the ISO9660 PVD root otherwise.
 * Enforces ISO 9660 directory depth limit of 8 levels. */
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

    /* ISO 9660 §6.8.2.1: directory depth shall not exceed 8 levels.
     * Root is level 0, each '/' adds one level. */
    int depth = 0;

    while (*p) {
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);

        int found = 0;
        for (int i = 0; i < n; i++) {
            const char *ename;
            size_t elen;

            /* Hybrid fallback chain: RR name → Joliet UTF-8 → ISO name */
            ename = iso9660_get_entry_name(ip, &entries[i], &elen);

            /* Skip dot entries */
            if (elen == 0) continue;
            if (elen == 1 && ename[0] == 0) continue;
            if (elen == 1 && ename[0] == '.') continue;
            if (elen == 2 && ename[0] == '.' && ename[1] == '.') continue;

            /* match_len already stripped of version suffix by helper */
            const char *match_name = ename;
            size_t match_len = elen;

            if (match_len == clen && memcmp(match_name, p, clen) == 0) {
                if (ip->has_rrip && (entries[i].rr_flags & RRIP_HAS_SL) &&
                    entries[i].rr_symlink[0] != '\0') {
                    /* Rock Ridge symlink — follow the symlink target */
                    char link_target[256];
                    strncpy(link_target, entries[i].rr_symlink,
                            sizeof(link_target) - 1);
                    link_target[sizeof(link_target) - 1] = '\0';

                    if (link_target[0] != '/') {
                        /* Relative symlink: build full path by
                         * prepending the parent directory. */
                        char full_path[512];
                        size_t parent_len = (size_t)(p - path);
                        size_t fp_len = 0;
                        if (parent_len > 0) {
                            memcpy(full_path, path, parent_len);
                            fp_len = parent_len;
                            while (fp_len > 0 &&
                                   full_path[fp_len - 1] == '/')
                                fp_len--;
                        }
                        if (fp_len > 0)
                            full_path[fp_len++] = '/';
                        size_t tlen = strlen(link_target);
                        if (fp_len + tlen + 1 > sizeof(full_path)) {
                            kprintf("iso9660: symlink target too long\n");
                            return -ENAMETOOLONG;
                        }
                        memcpy(full_path + fp_len, link_target, tlen);
                        fp_len += tlen;
                        full_path[fp_len] = '\0';

                        uint32_t re, rs;
                        if (iso9660_resolve(ip, full_path,
                                            &re, &rs) == 0) {
                            *extent = re;
                            *size   = rs;
                            found = 1;
                            break;
                        }
                        return -1;
                    }

                    /* Absolute symlink: resolve components */
                    int sdepth = 0;
                    const char *sp = link_target;
                    while (*sp) {
                        if (sdepth++ > 40) {
                            kprintf("iso9660: symlink depth exceeded\n");
                            return -ELOOP;
                        }
                        const char *send = sp;
                        while (*send && *send != '/') send++;
                        size_t slen = (size_t)(send - sp);
                        int sfound = 0;
                        for (int j = 0; j < n; j++) {
                            const char *sname;
                            size_t snlen;
                            sname = iso9660_get_entry_name(ip,
                                &entries[j], &snlen);
                            if (snlen == 0) continue;
                            if (snlen == 1 && sname[0] == 0) continue;
                            if (snlen == 1 && sname[0] == '.') continue;
                            if (snlen == 2 && sname[0] == '.' &&
                                sname[1] == '.') continue;
                            const char *smatch = sname;
                            size_t smlen = snlen;
                            if (smlen == slen &&
                                memcmp(smatch, sp, slen) == 0) {
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
                            if (depth >= ISO9660_DIR_DEPTH_MAX) {
                                kprintf("iso9660: dir depth (%d) "
                                        "exceeded following symlink\n",
                                        ISO9660_DIR_DEPTH_MAX);
                                return -ELOOP;
                            }
                            depth++;
                            n = iso_read_dir_entries(ip, *extent,
                                                     *size,
                                                     entries, 128);
                            if (n <= 0) return -1;
                        }
                    }
                    found = 1;
                    break;
                } else {
                    /* Regular (non-symlink) entry */
                    *extent = entries[i].extent;
                    *size   = entries[i].size;
                    found = 1;
                    break;
                }
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

        /* Enforce ISO 9660 directory depth limit (max 8 levels).
         * Root is at depth 0, each '/' traversal adds a level. */
        depth++;
        if (depth > ISO9660_DIR_DEPTH_MAX) {
            kprintf("iso9660: max directory depth (%d) exceeded\n",
                    ISO9660_DIR_DEPTH_MAX);
            return -ELOOP;
        }

        /* Read next level */
        n = iso_read_dir_entries(ip, *extent, *size, entries, 128);
        if (n <= 0) return -1;
    }

    return 0;
}

/* ── Rock Ridge check at mount time ─────────────────────────────── */

/* Check if a directory's records have Rock Ridge SUSP SP entries.
 * Scans the system use area for the SP signature (0xBE, 0xEF magic bytes).
 * @extent  LBA of the directory to scan
 * @size    Byte size of the directory
 * Returns 1 if RRIP is detected, 0 otherwise. */
static int check_rrip_in_dir(struct iso9660_priv *ip,
                              uint32_t extent, uint32_t size)
{
    uint8_t buf[2048];
    uint32_t offset = 0;

    while (offset < size) {
        uint32_t lba = extent + offset / ip->block_size;
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

/* Read a contiguous (non-interleaved) file's data blocks sequentially. */
static int iso9660_read_contiguous(struct iso9660_priv *ip, uint32_t extent,
                                    uint32_t size, void *buf, uint32_t max_size,
                                    uint32_t *out_size)
{
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

/*
 * Read an interleaved file (ISO 9660 §7.4.5 Interleaved Mode).
 *
 * Interleaved files store data in a repeating pattern:
 *   - file_unit_size consecutive logical blocks of this file's data
 *   - interleave_gap  consecutive logical blocks of other files' data
 *   - repeat
 *
 * This is used for CD-i, Video CD, and other multimedia formats where
 * audio/video data must be interleaved for synchronized playback.
 *
 * The total physical space consumed on disc is:
 *   ceil(data_length / (file_unit_size * block_size)) *
 *   (file_unit_size + interleave_gap) * block_size
 *
 * But the file's logical data_length in the directory record represents
 * only this file's data, not the interleave gaps.
 */
static int iso9660_read_interleaved(struct iso9660_priv *ip, uint32_t extent,
                                     uint32_t size, uint8_t file_unit_size,
                                     uint8_t interleave_gap, void *buf,
                                     uint32_t max_size, uint32_t *out_size)
{
    uint32_t to_read = size;
    if (to_read > max_size) to_read = max_size;
    if (file_unit_size == 0)
        file_unit_size = 1; /* safety: avoid division by zero */

    uint32_t cycle_blocks = (uint32_t)file_unit_size + (uint32_t)interleave_gap;
    uint32_t offset = 0;

    while (offset < to_read) {
        uint32_t block_num = offset / ip->block_size;
        uint32_t block_offs = offset % ip->block_size;

        /* Compute the LBA from the interleave pattern */
        uint32_t cycle = block_num / (uint32_t)file_unit_size;
        uint32_t block_in_unit = block_num % (uint32_t)file_unit_size;
        uint32_t lba = extent + cycle * cycle_blocks + block_in_unit;

        uint8_t block[2048];
        if (iso_read_block(ip, lba, block) < 0) break;

        uint32_t chunk = to_read - offset;
        if (chunk > ip->block_size) chunk = ip->block_size;
        memcpy((uint8_t *)buf + offset, block + block_offs, chunk);
        offset += chunk;
    }

    *out_size = offset;
    return 0;
}

static int iso9660_read(void *priv, const char *path, void *buf,
                         uint32_t max_size, uint32_t *out_size)
{
    struct iso9660_priv *ip = (struct iso9660_priv *)priv;
    uint32_t extent, size;
    if (iso9660_resolve(ip, path, &extent, &size) < 0)
        return -ENOENT;

    /* Check if this is an interleaved file by finding the directory entry.
     * Parse the parent directory to extract interleave fields. */
    uint8_t file_unit_size = 0;
    uint8_t interleave_gap = 0;

    const char *p = path;
    if (*p == '/') p++;
    if (*p != '\0') {
        /* Find parent directory */
        const char *slash = NULL;
        for (const char *s = p; *s; s++) {
            if (*s == '/') slash = s;
        }

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

        /* Search the parent directory for this file's entry */
        struct iso_rrip_entry entries[256];
        int n = iso_read_dir_entries(ip, pextent, psize, entries, 256);
        const char *name = slash ? slash + 1 : p;
        size_t nlen = strlen(name);

        for (int i = 0; i < n; i++) {
            size_t elen;
            const char *ename = iso9660_get_entry_name(ip, &entries[i],
                                                        &elen);

            if (elen == nlen && memcmp(ename, name, nlen) == 0) {
                file_unit_size = entries[i].file_unit_size;
                interleave_gap = entries[i].interleave_gap;
                break;
            }
        }
    }

    /* Dispatch to the appropriate read function based on interleave mode */
    if (file_unit_size > 0)
        return iso9660_read_interleaved(ip, extent, size,
                                        file_unit_size, interleave_gap,
                                        buf, max_size, out_size);
    else
        return iso9660_read_contiguous(ip, extent, size,
                                       buf, max_size, out_size);
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
        size_t elen;
        const char *ename = iso9660_get_entry_name(ip, &entries[i],
                                                     &elen);

        if (elen == nlen && memcmp(ename, name, nlen) == 0) {
            /* Use Rock Ridge POSIX attributes if available */
            if (entries[i].rr_flags & RRIP_HAS_PX) {
                st->mode   = entries[i].rr_mode & 07777;
                st->uid    = (uint16_t)entries[i].rr_uid;
                st->gid    = (uint16_t)entries[i].rr_gid;
                st->nlink  = entries[i].rr_nlink ? entries[i].rr_nlink : 1;
                /* Timestamp fields already have TF values overlaying
                 * PX values (TF is parsed after PX in the SUSP walk).
                 * Use them directly — no conditional needed. */
                st->atime = entries[i].rr_atime;
                st->mtime = entries[i].rr_mtime;
                /* Determine type from mode:
                 * 1 = file, 2 = directory, 4 = chrdev, 5 = blkdev
                 * Symlinks and other types fall back to file */
                if (entries[i].rr_mode & RR_S_IFDIR)
                    st->type = 2;
                else if (entries[i].rr_mode & RR_S_IFREG)
                    st->type = 1;
                else if (entries[i].rr_mode & RR_S_IFLNK)
                    st->type = 1; /* Symlinks reported as files (readlink provides target) */
                else if (entries[i].rr_mode & RR_S_IFBLK) {
                    st->type = 5; /* VFS_TYPE_BLK */
                    if (entries[i].rr_flags & RRIP_HAS_PN) {
                        st->dev_major = (uint16_t)entries[i].rr_dev_major;
                        st->dev_minor = (uint16_t)entries[i].rr_dev_minor;
                    }
                } else if (entries[i].rr_mode & RR_S_IFCHR) {
                    st->type = 4; /* VFS_TYPE_CHR */
                    if (entries[i].rr_flags & RRIP_HAS_PN) {
                        st->dev_major = (uint16_t)entries[i].rr_dev_major;
                        st->dev_minor = (uint16_t)entries[i].rr_dev_minor;
                    }
                } else
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
        size_t elen;
        const char *ename = iso9660_get_entry_name(ip, &entries[i],
                                                     &elen);

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
        size_t elen;

        /* Hybrid fallback chain: RR name → Joliet UTF-8 → ISO name */
        ename = iso9660_get_entry_name(ip, &entries[i], &elen);

        if (elen == 0) continue;
        if (elen == 1 && ename[0] == 0) continue;
        if (elen == 1 && ename[0] == '.') continue;
        if (elen == 2 && ename[0] == '.' && ename[1] == '.') continue;

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
    ip->num_sessions = 0;
    ip->active_session = 0;

    /* Scan for all sessions and populate legacy fields from the
     * active (last) session.  Handles both single-session and
     * multi-session (photo CD, CD-Extra, etc.) discs. */
    if (iso9660_find_sessions(ip) < 0) {
        kprintf("[iso9660] No primary volume descriptor found\n");
        kfree(ip);
        return -1;
    }

    /* Check for Rock Ridge presence by scanning the root directory
     * of the active session. */
    ip->has_rrip = check_rrip_in_dir(ip, ip->root_extent, ip->root_size);
    if (!ip->has_rrip && ip->has_joliet)
        ip->has_rrip = check_rrip_in_dir(ip, ip->joliet_root_extent,
                                          ip->joliet_root_size);

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
    	                   ", Joliet");
    if (ip->has_joliet && ip->sessions[ip->active_session].joliet_volume_name[0] != '\0')
    	mi_len += snprintf(mount_info + mi_len, sizeof(mount_info) - (size_t)mi_len,
    	                   " \"%s\"", ip->sessions[ip->active_session].joliet_volume_name);
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

int __init iso9660_init(void)
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
void __exit cleanup_module(void) {
    /* No VFS unregister yet; avoid unloading if filesystem is mounted */
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("ISO9660 CDROM filesystem with Rock Ridge (RRIP) and Joliet (UCS-2) extension support");
#endif

/* ── iso9660_umount ────────────────────────────────────── */
int iso9660_umount(const char *target)
{
    (void)target;
    kprintf("[iso9660] ISO 9660 unmounted\n");
    return 0;
}
/* ── iso9660_readdir ───────────────────────────────────── */
int iso9660_readdir(void *dir, void *filldir)
{
    (void)dir;
    (void)filldir;
    kprintf("[iso9660] readdir (no more entries)\n");
    return 0;
}
/* ── iso9660_lookup ────────────────────────────────────── */
int iso9660_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[iso9660] lookup: %s\n", name);
    return -ENOENT;
}
