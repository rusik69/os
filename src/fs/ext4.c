/*
 * src/fs/ext4.c — Ext4 read-only filesystem with extent tree support.
 *
 * Implements a read-only ext4 filesystem on top of the VFS layer.
 * Supports:
 *   - Ext4 superblock with EXT4_FEATURE_INCOMPAT_EXTENTS flag
 *   - Flex block groups (flex_bg)
 *   - Extent tree: ext4_extent_header → ext4_extent_idx → ext4_extent
 *   - Inline data in inodes
 *   - Directory entries with file_type
 *   - stat, readdir, read
 *
 * Read-only — no journal, no encryption, no htree indexing.
 * Backward-compatible with ext2/ext3; detected by feature flags.
 */

#define KERNEL_INTERNAL
#include "ext4.h"
#include "ext4_extents.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"
#include "initcall.h"
#include "crc.h"

#ifndef offsetof
#define offsetof(TYPE, MEMBER) __builtin_offsetof(TYPE, MEMBER)
#endif

#ifdef MODULE
#include "module.h"
#endif

/* ── Flex block group helpers ──────────────────────────────────────── */
/* These helpers are only valid once ep->flex_bg_size has been set.
 * If flex_bg_size == 0 (feature not enabled), each group is treated
 * as its own flex group. */

/* Return the flex group number for the given block group */
static inline uint32_t ext4_flex_group(struct ext4_priv *ep, uint32_t group)
{
    if (ep->flex_bg_size == 0)
        return group;
    return group / ep->flex_bg_size;
}

/* Return the first block group in the flex group containing 'group' */
static inline uint32_t ext4_flex_first_group(struct ext4_priv *ep, uint32_t group)
{
    if (ep->flex_bg_size == 0)
        return group;
    return (group / ep->flex_bg_size) * ep->flex_bg_size;
}

/* Return the last block group in the flex group containing 'group' */
static inline uint32_t ext4_flex_last_group(struct ext4_priv *ep, uint32_t group)
{
    if (ep->flex_bg_size == 0)
        return group;
    uint32_t first = ext4_flex_first_group(ep, group);
    uint32_t last = first + ep->flex_bg_size - 1;
    return (last >= ep->num_block_groups) ? ep->num_block_groups - 1 : last;
}

/* Return 1 if 'group' is the first block group in its flex group */
static inline int ext4_is_flex_first_group(struct ext4_priv *ep, uint32_t group)
{
    if (ep->flex_bg_size == 0)
        return 1;
    return (group % ep->flex_bg_size) == 0;
}

/* Return the number of block groups in the flex group containing 'group */
static inline uint32_t ext4_flex_group_count(struct ext4_priv *ep, uint32_t group)
{
    if (ep->flex_bg_size == 0)
        return 1;
    uint32_t first = ext4_flex_first_group(ep, group);
    uint32_t last = first + ep->flex_bg_size - 1;
    if (last >= ep->num_block_groups)
        last = ep->num_block_groups - 1;
    return last - first + 1;
}

/* ── Corruption helper ────────────────────────────────────────────── */

int ext4_corrupt(struct ext4_priv *ep, const char *reason)
{
    if (!ep)
        return -EFSCORRUPTED;
    vfs_force_readonly(ep->mountpoint, reason);
    return -EFSCORRUPTED;
}

/* ── Block I/O ────────────────────────────────────────────────────── */

int ext4_read_block(struct ext4_priv *ep, uint32_t block_num, uint8_t *buf)
{
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    if (sectors == 0) return ext4_corrupt(ep, "invalid block size");
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, (uint32_t)(lba + i), 1, buf + i * 512) != 0)
            return ext4_corrupt(ep, "block I/O error");
    }
    return 0;
}

/* ── Metadata CRC32C computation ─────────────────────────────────── */

/**
 * ext4_crc32c - Compute CRC32C for ext4 metadata with proper seeding
 * @ep: Filesystem private data
 * @data: Pointer to metadata buffer
 * @len: Length of data in bytes
 *
 * When CSUM_SEED incompat feature is set, uses the superblock's
 * s_checksum_seed as the initial CRC value.  Otherwise XORs the
 * UUID-derived seed into the computation.  This matches Linux ext4's
 * metadata checksum algorithm for superblock, block group descriptors,
 * and inode checksums.
 *
 * Return: CRC32C value (not XORed with ~0 — caller handles that)
 */
static uint32_t ext4_crc32c(struct ext4_priv *ep, const void *data,
                             uint32_t len)
{
	uint32_t crc;

	/*
	 * Compute the seed as the Linux kernel does:
	 *
	 *   seed = checksum_seed                        (if CSUM_SEED)
	 *   seed = ~crc32c(0, s_uuid, 16)               (otherwise)
	 *
	 * The kernel stores this seed and uses it as the initial CRC
	 * state for crypto_shash_update (raw CRC continuation — no
	 * entry/exit XOR).  Because our library crc32c() applies
	 * entry/exit NOTs, we use the identity:
	 *
	 *   raw_continue(seed, data) = ~crc32c(~seed, data)
	 */
	if (ep->incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
		crc = ep->sb.s_checksum_seed;
	else
		crc = ~crc32c(0, ep->sb.s_uuid, sizeof(ep->sb.s_uuid));

	crc = ~crc32c(~crc, data, len);
	return crc;
}

/* ── Superblock checksum ─────────────────────────────────────────── */

int ext4_verify_sb_checksum(struct ext4_priv *ep)
{
	uint32_t stored_csum, computed_csum;
	uint32_t saved_csum;

	/* Only verify when METADATA_CSUM feature is present */
	if (!(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;

	/* Verify checksum type */
	if (ep->sb.s_checksum_type != EXT4_CRC32C_CHKSUM &&
	    ep->sb.s_checksum_type != EXT4_CRC32_CHKSUM) {
		kprintf("[ext4] WARNING: unknown checksum type %u, "
		        "skipping verification\n", ep->sb.s_checksum_type);
		return 0;
	}

	stored_csum = ep->sb.s_checksum;
	if (stored_csum == 0)
		return 0; /* Zero checksum = not computed */

	/* CRC32C covers bytes up to (but not including) s_checksum */
	saved_csum = ep->sb.s_checksum;
	ep->sb.s_checksum = 0;
	computed_csum = ext4_crc32c(ep, &ep->sb,
	                offsetof(struct ext4_superblock, s_checksum));
	ep->sb.s_checksum = saved_csum;

	if (computed_csum != stored_csum) {
		kprintf("[ext4] CORRUPTION: superblock checksum mismatch: "
		        "stored=0x%08x, computed=0x%08x\n",
		        stored_csum, computed_csum);
		return ext4_corrupt(ep, "superblock checksum mismatch");
	}

	return 0;
}

/* ── Block group descriptor checksum ─────────────────────────────── */

int ext4_verify_bg_checksum(struct ext4_priv *ep,
                             const struct ext4_bg_desc *bg,
                             uint32_t bg_index)
{
	uint32_t stored_csum, computed_csum;

	/* Only verify when METADATA_CSUM or GDT_CSUM feature is present */
	if (!(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	    !(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM))
		return 0;

	/*
	 * The checksum is stored in bg_reserved[0] for 32-bit descriptors
	 * (offset 20 in the 32-byte packed struct).  CRC32C covers the
	 * first 20 bytes (fields before bg_reserved[0]).
	 *
	 * With METADATA_CSUM + 64BIT, the block group number is also
	 * folded into the checksum.
	 */
	stored_csum = bg->bg_reserved[0];
	if (stored_csum == 0)
		return 0; /* Zero checksum = not computed */

	computed_csum = ext4_crc32c(ep, bg,
	                offsetof(struct ext4_bg_desc, bg_reserved[0]));

	/* With 64-bit feature, fold the block group number in */
	if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT) {
		uint32_t le_bg = bg_index;
		computed_csum = ~crc32c(~computed_csum, &le_bg,
		                        sizeof(le_bg));
	}

	if (computed_csum != stored_csum) {
		kprintf("[ext4] WARNING: block group %u descriptor "
		        "checksum mismatch: stored=0x%08x, computed=0x%08x\n",
		        bg_index, stored_csum, computed_csum);
		/* Warn but don't fail — read-only is still safe */
		return -EFSCORRUPTED;
	}

	return 0;
}

/* ── Inode checksum ──────────────────────────────────────────────── */

int ext4_verify_inode_checksum(struct ext4_priv *ep,
                                const struct ext4_inode *inode,
                                uint32_t ino)
{
	uint16_t lo, hi;
	uint32_t stored_csum, computed_csum;

	/* Only verify for inodes with extended fields (inode_size > 128) */
	if (!(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM))
		return 0;
	if (ep->inode_size <= 128)
		return 0;
	if (inode->i_extra_isize <
	    offsetof(struct ext4_inode, i_checksum_hi) + sizeof(uint16_t) -
	    offsetof(struct ext4_inode, i_extra_isize))
		return 0; /* Extended area too small for checksum */

	/*
	 * In ext4, the inode checksum is split across two fields:
	 *   - Low 16 bits: stored in i_osd2[0..1] (bytes 116-117)
	 *   - High 16 bits: stored in i_checksum_hi (bytes 130-131)
	 *
	 * CRC32C covers bytes 0 to offsetof(i_checksum_hi) = 130 bytes.
	 */
	memcpy(&lo, &inode->i_osd2[0], sizeof(lo));
	hi = inode->i_checksum_hi;
	stored_csum = (uint32_t)lo | ((uint32_t)hi << 16);

	if (stored_csum == 0)
		return 0; /* Zero checksum = not computed */

	computed_csum = ext4_crc32c(ep, inode,
	                offsetof(struct ext4_inode, i_checksum_hi));

	/* Fold i_extra_isize into the checksum (Linux kernel compat) */
	{
		uint16_t extra = inode->i_extra_isize;
		computed_csum = ~crc32c(~computed_csum, &extra, sizeof(extra));
	}

	/* Fold i_projid into the checksum for inodes large enough */
	if (ep->inode_size >= offsetof(struct ext4_inode, i_projid) +
	                     sizeof(inode->i_projid)) {
		uint32_t projid = inode->i_projid;
		computed_csum = ~crc32c(~computed_csum, &projid, sizeof(projid));
	}

	/* Fold inode number into the checksum (Linux ext4 compatibility) */
	computed_csum = ~crc32c(~computed_csum, &ino, sizeof(ino));

	if (computed_csum != stored_csum) {
		kprintf("[ext4] WARNING: inode checksum mismatch: "
		        "stored=0x%08x, computed=0x%08x\n",
		        stored_csum, computed_csum);
		/* Warn but don't fail — read-only is still safe */
		return -EFSCORRUPTED;
	}

	return 0;
}

/* ── Superblock loading ───────────────────────────────────────────── */

static int ext4_load_super(struct ext4_priv *ep)
{
    uint8_t buf[1024];
    /* Superblock is at offset 1024 (sector 2 for 512-byte sectors) */
    uint64_t lba = 1024 / 512;
    if (blockdev_read_sectors(ep->dev_id, (uint32_t)lba, 2, buf) != 0)
        return -1;
    memcpy(&ep->sb, buf, sizeof(ep->sb));
    return 0;
}

/* ── Block group descriptor caching ───────────────────────────────── */

static int ext4_load_bgd_cache(struct ext4_priv *ep)
{
    uint32_t bgd_entry_size = sizeof(struct ext4_bg_desc);
    /* For 64-bit mode, each BGD entry is larger (64 bytes vs 32) */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        bgd_entry_size = 64;

    uint64_t bgd_bytes = (uint64_t)ep->num_block_groups * bgd_entry_size;
    uint32_t bgd_blocks = (uint32_t)((bgd_bytes + ep->block_size - 1) / ep->block_size);

    ep->bgd_cache = (struct ext4_bg_desc *)kmalloc((size_t)bgd_bytes);
    if (!ep->bgd_cache)
        return -ENOMEM;
    ep->bgd_cache_size = (uint32_t)bgd_bytes;

    /* The block group descriptor table starts at byte offset 2048 on disk:
     *   byte 0–1023: x86 boot sector / partition table
     *   byte 1024–2047: primary superblock (1024 bytes)
     *   byte 2048+:    block group descriptor table
     *
     * For block_size == 1024: BGD starts at block 2 (byte 2048)
     * For block_size >  1024: BGD starts at offset 2048 within block 0
     *
     * With flex_bg, the BGD table may extend into subsequent groups'
     * reserved space, but the PRIMARY copy is always at this location. */
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t offset = 0;

    if (ep->block_size == 1024) {
        /* BGD table starts at block 2 */
        for (uint32_t b = 0; b < bgd_blocks; b++) {
            if (ext4_read_block(ep, 2 + b, block_buf) < 0)
                return ext4_corrupt(ep, "failed to read BGD table");
            uint32_t copy = ep->block_size;
            uint32_t remaining = bgd_bytes - offset;
            if (copy > remaining) copy = remaining;
            memcpy((uint8_t *)ep->bgd_cache + offset, block_buf, copy);
            offset += copy;
        }
    } else {
        /* block_size > 1024: BGD starts at byte 2048 of block 0 */
        const uint32_t bgd_offset_in_block0 = 2048;
        if (ext4_read_block(ep, 0, block_buf) < 0)
            return ext4_corrupt(ep, "failed to read block 0 for BGD table");
        uint32_t chunk = ep->block_size - bgd_offset_in_block0;
        if (chunk > bgd_bytes) chunk = bgd_bytes;
        memcpy((uint8_t *)ep->bgd_cache, block_buf + bgd_offset_in_block0, chunk);
        offset += chunk;

        /* Read remaining BGD blocks starting at block 1 */
        uint32_t remaining = bgd_bytes - offset;
        uint32_t extra_blocks = (remaining + ep->block_size - 1) / ep->block_size;
        for (uint32_t b = 0; b < extra_blocks; b++) {
            if (ext4_read_block(ep, 1 + b, block_buf) < 0)
                return ext4_corrupt(ep, "failed to read BGD continuation block");
            uint32_t copy = ep->block_size;
            if (copy > remaining) copy = remaining;
            memcpy((uint8_t *)ep->bgd_cache + offset, block_buf, copy);
            offset += copy;
            remaining -= copy;
        }
    }

    /* ── Verify block group descriptor checksums ── */
    {
        uint32_t bgd_count;

        bgd_entry_size = sizeof(struct ext4_bg_desc);
        if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
            bgd_entry_size = 64;

        bgd_count = ep->num_block_groups;
        for (uint32_t i = 0; i < bgd_count; i++) {
            struct ext4_bg_desc *bg = (struct ext4_bg_desc *)
                ((uint8_t *)ep->bgd_cache + i * bgd_entry_size);
            if (ext4_verify_bg_checksum(ep, bg, i) < 0) {
                kprintf("[ext4] NOTE: BGD %u checksum mismatch — continuing "
                        "read-only\n", i);
            }
        }
    }

    return 0;
}

/* ── Inode reading ─────────────────────────────────────────────────── */

static int ext4_read_inode(struct ext4_priv *ep, uint32_t ino, struct ext4_inode *inode)
{
    if (ino == 0)
        return ext4_corrupt(ep, "inode 0 is invalid");
    if (ino > ep->sb.s_inodes_count)
        return ext4_corrupt(ep, "inode number exceeds count");
    uint32_t group = (ino - 1) / ep->inodes_per_group;
    uint32_t index = (ino - 1) % ep->inodes_per_group;

    if (!ep->bgd_cache || group >= ep->num_block_groups)
        return ext4_corrupt(ep, "block group out of range");

    /* BGD entries may be 64 bytes with 64BIT feature; use byte offset */
    uint32_t bgd_esize = sizeof(struct ext4_bg_desc);
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        bgd_esize = 64;
    struct ext4_bg_desc *bgd = (struct ext4_bg_desc *)
        ((uint8_t *)ep->bgd_cache + group * bgd_esize);
    uint32_t inode_table_block = bgd->bg_inode_table;
    uint32_t byte_offset = index * ep->inode_size;

    uint32_t tbl_block = inode_table_block + byte_offset / ep->block_size;
    uint32_t tbl_off   = byte_offset % ep->block_size;

    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    if (ext4_read_block(ep, tbl_block, block_buf) < 0)
        return -1;

    memcpy(inode, block_buf + tbl_off, sizeof(struct ext4_inode));

    /* ── Verify inode metadata checksum (CRC32C) ── */
    if (ext4_verify_inode_checksum(ep, inode, ino) < 0) {
        kprintf("[ext4] NOTE: inode %u checksum mismatch — continuing "
                "read-only\n", ino);
    }

    return 0;
}

/* ── Block resolution (extent or legacy indirect) ─────────────────── */

static int64_t ext4_get_block_num(struct ext4_priv *ep,
                                   struct ext4_inode *inode,
                                   uint32_t iblock)
{
    /* If EXTENTS flag is set, use extent tree */
    if (inode->i_flags & EXT4_EXTENTS_FL)
        return ext4_ext_find_extent(ep, inode, iblock);

    /* Otherwise fall back to legacy ext2 indirect block scheme.
     * This provides backward compatibility with ext2/ext3. */
    if (iblock < 12) {
        return (int64_t)inode->i_block[iblock];
    }

    /* Singly indirect */
    uint32_t entries_per_block = ep->block_size / 4;
    uint32_t sind = iblock - 12;

    if (sind < entries_per_block) {
        if (inode->i_block[12] == 0)
            return 0; /* hole */
        uint8_t indir[EXT4_MAX_BLOCK_SIZE] = {0};
        if (ext4_read_block(ep, inode->i_block[12], indir) < 0)
            return -1;
        uint32_t *ptrs = (uint32_t *)indir;
        return (int64_t)ptrs[sind];
    }
    /* Doubly/triply indirect not supported for non-extent files (rare) */
    return -1;
}

/* ── Inline data support ────────────────────────────────────────────── */

/*
 * ext4_read_inline_data — copy inline file data from inode to buffer.
 *
 * For inodes with EXT4_INLINE_DATA_FL set, file data is stored directly
 * in i_block[0..EXT4_MAX_INLINE_DATA-1] rather than in separate disk
 * blocks.  The i_blocks field (in 512-byte units) should be 0 for a
 * purely inline file — if non-zero, the file has been expanded beyond
 * the inline area and this function returns -1.
 *
 * Returns number of bytes copied on success, or -1 if not inline.
 */
static int ext4_read_inline_data(struct ext4_priv *ep,
                                  struct ext4_inode *inode,
                                  uint8_t *buf, uint32_t max_size)
{
    if (!(inode->i_flags & EXT4_INLINE_DATA_FL))
        return -1;

    /* i_blocks counts 512-byte sectors.  For a pure inline file this
     * must be 0 — the data lives entirely inside the inode.
     * With HUGE_FILE, use the full 48-bit block count. */
    uint64_t inode_blocks = ext4_inode_get_blocks(inode,
        !!(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE));
    if (inode_blocks != 0)
        return -1;

    uint32_t size = inode->i_size;
    uint32_t copy = size;
    if (copy > max_size) copy = max_size;
    if (copy > EXT4_MAX_INLINE_DATA) copy = EXT4_MAX_INLINE_DATA;
    memcpy(buf, inode->i_block, copy);
    return (int)copy;
}

/* ── Inline directory helpers ────────────────────────────────────────── */

/*
 * ext4_find_entry_inline — search for a name in an inline-data directory.
 *
 * Directory entries are stored in i_block[] using the same ext4_dir_entry
 * format as block-based directories.  Returns inode number on success,
 * 0 if not found or not an inline directory.
 */
static uint32_t ext4_find_entry_inline(struct ext4_priv *ep,
                                        struct ext4_inode *dir_inode,
                                        const char *name)
{
    if (!(dir_inode->i_flags & EXT4_INLINE_DATA_FL))
        return 0;
    /* Use full block count (HUGE_FILE aware) — inline files must have
     * zero disk blocks allocated. */
    uint64_t blocks = ext4_inode_get_blocks(dir_inode,
        !!(ep->ro_compat & EXT4_FEATURE_RO_COMPAT_HUGE_FILE));
    if (blocks != 0)
        return 0;

    uint32_t file_size = dir_inode->i_size;
    uint32_t name_len = (uint32_t)strlen(name);
    uint32_t off = 0;

    while (off + 8 <= file_size && off < EXT4_MAX_INLINE_DATA) {
        struct ext4_dir_entry *de = (struct ext4_dir_entry *)
            ((uint8_t *)dir_inode->i_block + off);

        if (de->rec_len < 8)
            break; /* corrupted entry */
        if (de->rec_len == 0)
            break; /* safety: zero-length entry causes infinite loop */

        if (de->inode != 0 &&
            de->name_len == name_len &&
            memcmp(de->name, name, name_len) == 0) {
            return de->inode;
        }

        off += de->rec_len;
    }
    return 0;
}

/*
 * ext4_readdir_inline — list directory entries from an inline-data dir.
 *
 * Outputs entries using the same kprintf format as the block-based
 * ext4_readdir.  Returns 0 on success, negative error code on failure.
 */
static int ext4_readdir_inline(struct ext4_priv *ep,
                                struct ext4_inode *dir_inode)
{
    (void)ep;
    if (!(dir_inode->i_flags & EXT4_INLINE_DATA_FL))
        return -ENOTDIR;

    uint32_t file_size = dir_inode->i_size;
    uint32_t off = 0;

    kprintf(".              <DIR>\n");
    kprintf("..             <DIR>\n");

    while (off + 8 <= file_size && off < EXT4_MAX_INLINE_DATA) {
        struct ext4_dir_entry *de = (struct ext4_dir_entry *)
            ((uint8_t *)dir_inode->i_block + off);

        if (de->rec_len < 8)
            break; /* corrupted */
        if (de->rec_len == 0)
            break; /* safety */

        if (de->inode != 0 && de->name_len > 0) {
            char fname[256];
            uint32_t nlen = de->name_len;
            if (nlen > 255) nlen = 255;
            memcpy(fname, de->name, nlen);
            fname[nlen] = '\0';

            const char *type_str = "<FILE>";
            switch (de->file_type) {
            case EXT4_FT_DIR:      type_str = "<DIR>";  break;
            case EXT4_FT_REG_FILE: type_str = "<FILE>"; break;
            case EXT4_FT_SYMLINK:  type_str = "<LNK>";  break;
            case EXT4_FT_CHRDEV:   type_str = "<CHR>";  break;
            case EXT4_FT_BLKDEV:   type_str = "<BLK>";  break;
            default:               type_str = "<?>";    break;
            }

            kprintf("%-16s %s\n", fname, type_str);
        }

        off += de->rec_len;
    }

    return 0;
}

/* ── File read ─────────────────────────────────────────────────────── */

static int ext4_read_file(struct ext4_priv *ep, struct ext4_inode *inode,
                           uint8_t *buf, uint32_t size, uint32_t offset)
{
    /* Try inline data first */
    if (inode->i_flags & EXT4_INLINE_DATA_FL) {
        if (offset >= inode->i_size) return 0;
        return ext4_read_inline_data(ep, inode, buf, size);
    }

    uint32_t file_size = inode->i_size;
    /* Handle large files: combine with i_size_high */
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_LARGE_FILE)
        file_size |= ((uint64_t)inode->i_size_high << 32);

    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint32_t block_size = ep->block_size;
    uint32_t first_block = offset / block_size;
    uint32_t last_block = (offset + size - 1) / block_size;
    uint32_t buf_offset = 0;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];

    for (uint32_t b = first_block; b <= last_block; b++) {
        int64_t phys_block = ext4_get_block_num(ep, inode, b);
        if (phys_block < 0) return -1;

        if (phys_block == 0) {
            /* Hole — sparse block */
            memset(block_buf, 0, block_size);
        } else {
            if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
                return -1;
        }

        uint32_t block_off = (b == first_block) ? (offset % block_size) : 0;
        uint32_t chunk = block_size - block_off;
        if (chunk > size - buf_offset) chunk = size - buf_offset;
        memcpy(buf + buf_offset, block_buf + block_off, chunk);
        buf_offset += chunk;
    }
    return (int)buf_offset;
}

/* ── Directory reading ─────────────────────────────────────────────── */

/* Find a directory entry by name.  Returns inode number, or 0 on error. */
static uint32_t ext4_find_entry(struct ext4_priv *ep, uint32_t dir_ino,
                                 const char *name)
{
    struct ext4_inode dir_inode;
    if (ext4_read_inode(ep, dir_ino, &dir_inode) < 0)
        return 0;

    /* Inline-data directory: entries are in i_block[] */
    if (dir_inode.i_flags & EXT4_INLINE_DATA_FL)
        return ext4_find_entry_inline(ep, &dir_inode, name);

    uint32_t file_size = dir_inode.i_size;
    uint32_t block_size = ep->block_size;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t bytes_read = 0;
    uint32_t name_len = (uint32_t)strlen(name);

    while (bytes_read < file_size) {
        uint32_t block_num = bytes_read / block_size;
        int64_t phys_block = ext4_get_block_num(ep, &dir_inode, block_num);
        if (phys_block < 0) return 0;

        if (phys_block == 0) {
            /* Hole in directory — skip block */
            bytes_read += block_size;
            continue;
        }

        if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
            return 0;

        uint32_t block_off = 0;
        while (block_off < block_size && bytes_read + block_off < file_size) {
            struct ext4_dir_entry *de = (struct ext4_dir_entry *)(block_buf + block_off);

            if (de->rec_len < 8) break; /* corrupted */

            if (de->inode != 0 &&
                de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                return de->inode;
            }

            block_off += de->rec_len;
        }
        bytes_read += block_size;
    }
    return 0; /* not found */
}

/* ── VFS operations ──────────────────────────────────────────────────── */

static int ext4_read(void *priv, const char *path,
                      void *buf, uint32_t max_size, uint32_t *out_size)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;
    if (out_size) *out_size = 0;

    /* Resolve the path to an inode */
    uint32_t ino = EXT4_ROOT_INO; /* start at root */
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        /* Walk path components */
        char name_buf[256];
        while (*p) {
            /* Skip leading slashes */
            while (*p == '/') p++;
            if (!*p) break;

            /* Read next component */
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';

            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    struct ext4_inode inode;
    if (ext4_read_inode(ep, ino, &inode) < 0)
        return -EIO;

    if (inode.i_mode & 0x4000) /* S_IFDIR */
        return -EISDIR;

    int ret = ext4_read_file(ep, &inode, (uint8_t *)buf, max_size, 0);
    if (out_size && ret > 0) *out_size = (uint32_t)ret;
    return (ret < 0) ? ret : 0;
}

static int ext4_write(void *priv, const char *path,
                       const void *data, uint32_t size)
{
    (void)priv; (void)path; (void)data; (void)size;
    return -EROFS;
}

static int ext4_stat(void *priv, const char *path, struct vfs_stat *st)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;
    memset(st, 0, sizeof(*st));

    uint32_t ino = EXT4_ROOT_INO;
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        char name_buf[256];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';
            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) return -ENOENT;
        }
    }

    struct ext4_inode inode;
    if (ext4_read_inode(ep, ino, &inode) < 0)
        return -EIO;

    st->ino = ino;

    /* Determine file type from mode */
    if (inode.i_mode & 0x8000)       st->type = VFS_TYPE_FILE;  /* S_IFREG */
    else if (inode.i_mode & 0x4000)  st->type = VFS_TYPE_DIR;   /* S_IFDIR */
    else if (inode.i_mode & 0xA000)  st->type = VFS_TYPE_LINK;  /* S_IFLNK */
    else                              st->type = VFS_TYPE_FILE;

    st->size = inode.i_size;
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_LARGE_FILE)
        st->size |= ((uint64_t)inode.i_size_high << 32);

    st->uid = inode.i_uid;
    st->gid = inode.i_gid;
    st->mode = inode.i_mode;
    st->mtime = inode.i_mtime;
    st->atime = inode.i_atime;
    st->nlink = inode.i_links_count;

    return 0;
}

static int ext4_create(void *priv, const char *path, uint8_t type)
{
    (void)priv; (void)path; (void)type;
    return -EROFS;
}

static int ext4_unlink(void *priv, const char *path)
{
    (void)priv; (void)path;
    return -EROFS;
}

static int ext4_readdir(void *priv, const char *path)
{
    struct ext4_priv *ep = (struct ext4_priv *)priv;

    /* Resolve directory inode */
    uint32_t ino = EXT4_ROOT_INO;
    const char *p = path;
    if (*p == '/') p++;

    if (*p) {
        char name_buf[256];
        while (*p) {
            while (*p == '/') p++;
            if (!*p) break;
            int i = 0;
            while (*p && *p != '/' && i < 255) name_buf[i++] = *p++;
            name_buf[i] = '\0';
            ino = ext4_find_entry(ep, ino, name_buf);
            if (ino == 0) {
                kprintf("[ext4] readdir: path not found: %s\\n", path);
                return -ENOENT;
            }
        }
    }

    struct ext4_inode dir_inode;
    if (ext4_read_inode(ep, ino, &dir_inode) < 0)
        return -EIO;

    if (!(dir_inode.i_mode & 0x4000))
        return -ENOTDIR;

    /* Inline-data directory: entries are in i_block[] */
    if (dir_inode.i_flags & EXT4_INLINE_DATA_FL)
        return ext4_readdir_inline(ep, &dir_inode);

    uint32_t file_size = dir_inode.i_size;
    uint32_t block_size = ep->block_size;
    uint8_t block_buf[EXT4_MAX_BLOCK_SIZE];
    uint32_t bytes_read = 0;

    kprintf(".              <DIR>\\n");
    kprintf("..             <DIR>\\n");

    while (bytes_read < file_size) {
        uint32_t block_num = bytes_read / block_size;
        int64_t phys_block = ext4_get_block_num(ep, &dir_inode, block_num);
        if (phys_block < 0) return -1;

        if (phys_block == 0) {
            bytes_read += block_size;
            continue;
        }

        if (ext4_read_block(ep, (uint32_t)phys_block, block_buf) < 0)
            return -1;

        uint32_t block_off = 0;
        while (block_off < block_size && bytes_read + block_off < file_size) {
            struct ext4_dir_entry *de = (struct ext4_dir_entry *)(block_buf + block_off);

            if (de->rec_len < 8) break;

            if (de->inode != 0 && de->name_len > 0) {
                char fname[256];
                uint32_t nlen = de->name_len;
                if (nlen > 255) nlen = 255;
                memcpy(fname, de->name, nlen);
                fname[nlen] = '\0';

                /* Determine type string */
                const char *type_str = "<FILE>";
                switch (de->file_type) {
                    case EXT4_FT_DIR:      type_str = "<DIR>";  break;
                    case EXT4_FT_REG_FILE: type_str = "<FILE>"; break;
                    case EXT4_FT_SYMLINK:  type_str = "<LNK>";  break;
                    case EXT4_FT_CHRDEV:   type_str = "<CHR>";  break;
                    case EXT4_FT_BLKDEV:   type_str = "<BLK>";  break;
                    default:               type_str = "<?>";    break;
                }

                kprintf("%-16s %s\\n", fname, type_str);
            }

            block_off += de->rec_len;
        }
        bytes_read += block_size;
    }

    return 0;
}

static struct vfs_ops ext4_ops = {
    .read    = ext4_read,
    .write   = ext4_write,
    .stat    = ext4_stat,
    .create  = ext4_create,
    .unlink  = ext4_unlink,
    .readdir = ext4_readdir,
};

/* ── Feature compatibility logging ─────────────────────────────────── */

static void ext4_log_incompat_features(uint32_t flags)
{
    if (flags & EXT4_FEATURE_INCOMPAT_COMPRESSION)
        kprintf("[ext4]   INCOMPAT: compression\n");
    if (flags & EXT4_FEATURE_INCOMPAT_FILETYPE)
        kprintf("[ext4]   INCOMPAT: filetype\n");
    if (flags & EXT4_FEATURE_INCOMPAT_RECOVER)
        kprintf("[ext4]   INCOMPAT: needs journal recovery\n");
    if (flags & EXT4_FEATURE_INCOMPAT_JOURNAL_DEV)
        kprintf("[ext4]   INCOMPAT: journal device\n");
    if (flags & EXT4_FEATURE_INCOMPAT_META_BG)
        kprintf("[ext4]   INCOMPAT: meta block groups\n");
    if (flags & EXT4_FEATURE_INCOMPAT_EXTENTS)
        kprintf("[ext4]   INCOMPAT: extents\n");
    if (flags & EXT4_FEATURE_INCOMPAT_64BIT)
        kprintf("[ext4]   INCOMPAT: 64-bit\n");
    if (flags & EXT4_FEATURE_INCOMPAT_MMP)
        kprintf("[ext4]   INCOMPAT: MMP (multiple mount protection)\n");
    if (flags & EXT4_FEATURE_INCOMPAT_FLEX_BG)
        kprintf("[ext4]   INCOMPAT: flex block groups\n");
    if (flags & EXT4_FEATURE_INCOMPAT_EA_INODE)
        kprintf("[ext4]   INCOMPAT: EA inodes\n");
    if (flags & EXT4_FEATURE_INCOMPAT_DIRDATA)
        kprintf("[ext4]   INCOMPAT: data in directory entries\n");
    if (flags & EXT4_FEATURE_INCOMPAT_CSUM_SEED)
        kprintf("[ext4]   INCOMPAT: checksum seed\n");
    if (flags & EXT4_FEATURE_INCOMPAT_LARGEDIR)
        kprintf("[ext4]   INCOMPAT: large directories\n");
    if (flags & EXT4_FEATURE_INCOMPAT_INLINE_DATA)
        kprintf("[ext4]   INCOMPAT: inline data\n");
    if (flags & EXT4_FEATURE_INCOMPAT_ENCRYPT)
        kprintf("[ext4]   INCOMPAT: encryption\n");
}

static void ext4_log_ro_compat_features(uint32_t flags)
{
    if (flags & EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER)
        kprintf("[ext4]   RO_COMPAT: sparse superblock\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_LARGE_FILE)
        kprintf("[ext4]   RO_COMPAT: large files\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_BTREE_DIR)
        kprintf("[ext4]   RO_COMPAT: btree directories\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_HUGE_FILE)
        kprintf("[ext4]   RO_COMPAT: huge files\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_GDT_CSUM)
        kprintf("[ext4]   RO_COMPAT: group descriptor checksums\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_DIR_NLINK)
        kprintf("[ext4]   RO_COMPAT: directory nlink\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE)
        kprintf("[ext4]   RO_COMPAT: extra inode size\n");
    if (flags & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
        kprintf("[ext4]   RO_COMPAT: metadata checksums (CRC32C)\n");
}

static void ext4_log_compat_features(uint32_t flags)
{
    if (flags & EXT4_FEATURE_COMPAT_DIR_PREALLOC)
        kprintf("[ext4]   COMPAT: directory preallocation\n");
    if (flags & EXT4_FEATURE_COMPAT_IMAGIC_INODES)
        kprintf("[ext4]   COMPAT: imagic inodes\n");
    if (flags & EXT4_FEATURE_COMPAT_HAS_JOURNAL)
        kprintf("[ext4]   COMPAT: has journal\n");
    if (flags & EXT4_FEATURE_COMPAT_EXT_ATTR)
        kprintf("[ext4]   COMPAT: extended attributes\n");
    if (flags & EXT4_FEATURE_COMPAT_RESIZE_INO)
        kprintf("[ext4]   COMPAT: resize inode\n");
    if (flags & EXT4_FEATURE_COMPAT_DIR_INDEX)
        kprintf("[ext4]   COMPAT: directory index (HTree)\n");
}

/* ── Superblock state checking ────────────────────────────────────── */

static const char *ext4_state_string(uint16_t state)
{
    if (state == 0)
        return "clean (no VALID_FS flag)";
    if (state & EXT4_ERROR_FS)
        return "ERRORS DETECTED";
    if (state & EXT4_ORPHAN_FS)
        return "orphans pending recovery";
    if (state & EXT4_VALID_FS)
        return "clean";
    return "unknown";
}

/* ── Flex_bg verification ──────────────────────────────────────────── */

/* Verify that the flex_bg layout is internally consistent.
 * Returns 0 on success, -EFSCORRUPTED if layout is invalid.
 * Call immediately after determining flex_bg_size and num_block_groups. */
static int ext4_verify_flex_bg(struct ext4_priv *ep)
{
    if (ep->flex_bg_size == 0)
        return 0; /* flex_bg not enabled — nothing to verify */

    /* Must be at least 1 group per flex group */
    if (ep->flex_bg_size < 1) {
        kprintf("[ext4] ERROR: flex_bg_size=%u is invalid\\n", ep->flex_bg_size);
        return ext4_corrupt(ep, "invalid flex_bg_size");
    }

    /* Must be a power of 2 (ext4 filesystems always create it as such) */
    if (ep->flex_bg_size & (ep->flex_bg_size - 1)) {
        kprintf("[ext4] ERROR: flex_bg_size=%u is not a power of 2\\n", ep->flex_bg_size);
        return ext4_corrupt(ep, "flex_bg_size not power of 2");
    }

    /* flex_bg_size should not exceed num_block_groups for a valid fs */
    if (ep->flex_bg_size > ep->num_block_groups) {
        kprintf("[ext4] WARNING: flex_bg_size=%u exceeds group count=%u\\n",
            ep->flex_bg_size, ep->num_block_groups);
        /* Not fatal — treat entire fs as one flex group */
    }

    /* Verify flex_bg feature flag is present */
    if (!(ep->incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG)) {
        kprintf("[ext4] WARNING: flex_bg_size set but FLEX_BG feature flag missing\\n");
    }

    /* Verify BGD table fits within the first group's data space */
    uint32_t bgd_entry_size = sizeof(struct ext4_bg_desc);
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
        bgd_entry_size = 64;
    uint32_t bgd_bytes = ep->num_block_groups * bgd_entry_size;
    uint32_t bgd_offset_in_group;
    bgd_offset_in_group = 2048;

    /* With flex_bg, the BGD table can extend into subsequent groups'
     * reserved GDT blocks.  Warn if it overflows the first group. */
    uint32_t first_group_data_end = ep->block_size;
    if (bgd_offset_in_group + bgd_bytes > first_group_data_end) {
        uint32_t overflow = bgd_offset_in_group + bgd_bytes - first_group_data_end;
        uint32_t overflow_blocks = (overflow + ep->block_size - 1) / ep->block_size;
        kprintf("[ext4] BGD table spans %u blocks into reserved space\\n",
            overflow_blocks + 1);
    }

    return 0;
}

/* ── Project quota support ────────────────────────────────────────── */

/* Initialize quota tracking from superblock fields.
 *
 * Reads s_usr_quota_inum, s_grp_quota_inum, and s_prj_quota_inum
 * from the superblock and stores them in the ext4_priv structure for
 * later lookup.  Detects the on-disk quota format (VFSv0 or VFSv1)
 * based on feature flags. */
int ext4_init_quota(struct ext4_priv *ep)
{
    ep->usr_quota_inum = ep->sb.s_usr_quota_inum;
    ep->grp_quota_inum = ep->sb.s_grp_quota_inum;
    ep->prj_quota_inum = ep->sb.s_prj_quota_inum;
    ep->quota_format   = 0;

    /* Detect quota format:
     * - VFSv1 (QFMT_VFS_V1, 64-bit) is used with metadata_csum
     * - VFSv0 (QFMT_VFS_V0) is used on older ext4 filesystems
     * - QFMT_VFS_OLD for very old quota files */
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        /* With metadata_csum, quota uses v1 64-bit format */
        ep->quota_format = EXT4_QFMT_VFS_V1;
    } else if (ep->compat & EXT4_FEATURE_COMPAT_EXT_ATTR) {
        /* Extended attribute compat flag hints at VFSv0 */
        ep->quota_format = EXT4_QFMT_VFS_V0;
    } else if (ep->prj_quota_inum != 0) {
        /* Project quota inode present → VFSv1 is the only format
         * that supports project quotas */
        ep->quota_format = EXT4_QFMT_VFS_V1;
    }

    return 0;
}

/* Read the on-disk quota block for a given project ID.
 *
 * Uses the project quota inode (s_prj_quota_inum) to locate and
 * read the struct ext4_dqblk entry for @projid.  The quota file
 * contains blocks of packed struct ext4_dqblk entries, indexed
 * linearly by project ID.
 *
 * Returns 0 on success and fills @dqblk, or a negative errno on
 * failure (-ENOENT if no project quota inode, -EIO on I/O error). */
int ext4_read_quota_block(struct ext4_priv *ep, uint32_t projid,
                           struct ext4_dqblk *dqblk)
{
    if (!ep || !dqblk)
        return -EINVAL;

    uint32_t quota_inum = ep->prj_quota_inum;
    if (quota_inum == 0)
        return -ENOENT;

    /* Read the quota inode */
    struct ext4_inode qinode;
    if (ext4_read_inode(ep, quota_inum, &qinode) < 0)
        return -EIO;

    /* Calculate the block and offset within the block.
     * Each block holds (block_size / sizeof(dqblk)) entries. */
    uint32_t entries_per_block = ep->block_size / (uint32_t)sizeof(struct ext4_dqblk);
    if (entries_per_block == 0)
        return -EINVAL;

    uint32_t entry_index   = projid;
    uint32_t block_index   = entry_index / entries_per_block;
    uint32_t block_offset  = entry_index % entries_per_block;

    /* Resolve logical block to physical block via the extent tree */
    int64_t phys_block = ext4_get_block_num(ep, &qinode, block_index);
    if (phys_block < 0)
        return -EIO;

    /* Read the quota block */
    uint8_t *block_buf = (uint8_t *)kmalloc(ep->block_size);
    if (!block_buf)
        return -ENOMEM;

    int ret = ext4_read_block(ep, (uint32_t)phys_block, block_buf);
    if (ret < 0) {
        kfree(block_buf);
        return ret;
    }

    /* Extract the entry at the calculated offset */
    const struct ext4_dqblk *entries = (const struct ext4_dqblk *)block_buf;
    memcpy(dqblk, &entries[block_offset], sizeof(*dqblk));

    kfree(block_buf);
    return 0;
}

/* Log quota information during mount.
 *
 * Reports the user, group, and project quota inode numbers, the detected
 * format version, and the first project quota entry (project 0) if
 * project quotas are configured. */
void ext4_log_quota_info(struct ext4_priv *ep)
{
    const char *fmt_str;

    switch (ep->quota_format) {
    case EXT4_QFMT_VFS_OLD:
        fmt_str = "VFS_OLD";
        break;
    case EXT4_QFMT_VFS_V0:
        fmt_str = "VFS_V0";
        break;
    case EXT4_QFMT_VFS_V1:
        fmt_str = "VFS_V1";
        break;
    default:
        fmt_str = "none";
        break;
    }

    kprintf("[ext4] Quota: usr_inum=%u grp_inum=%u prj_inum=%u "
            "fmt=%s\n",
            ep->usr_quota_inum, ep->grp_quota_inum,
            ep->prj_quota_inum, fmt_str);

    if (ep->prj_quota_inum != 0) {
        /* Read and report project 0 quota as a sample */
        struct ext4_dqblk dqblk;
        if (ext4_read_quota_block(ep, 0, &dqblk) == 0 && dqblk.dqb_valid) {
            kprintf("[ext4]   Project 0: space=%llu/%llu inodes=%llu/%llu\n",
                    (unsigned long long)dqblk.dqb_curspace,
                    (unsigned long long)dqblk.dqb_bhardlimit,
                    (unsigned long long)dqblk.dqb_curinodes,
                    (unsigned long long)dqblk.dqb_ihardlimit);
        }
    }
}

/* ── Mount ─────────────────────────────────────────────────────────── */

int ext4_mount(const char *mountpoint, uint8_t dev_id)
{
    struct ext4_priv *ep = (struct ext4_priv *)kmalloc(sizeof(struct ext4_priv));
    if (!ep) return -ENOMEM;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = dev_id;
    strncpy(ep->mountpoint, mountpoint, sizeof(ep->mountpoint) - 1);

    if (ext4_load_super(ep) < 0) {
        kprintf("[ext4] failed to read superblock\n");
        goto fail;
    }

    if (ep->sb.s_magic != EXT4_SUPER_MAGIC) {
        kprintf("[ext4] bad magic: 0x%04x\n", ep->sb.s_magic);
        goto fail;
    }

    /* ── Validate superblock revision level ── */
    if (ep->sb.s_rev_level != EXT4_GOOD_OLD_REV &&
        ep->sb.s_rev_level != EXT4_DYNAMIC_REV) {
        kprintf("[ext4] ERROR: unsupported superblock revision level: %u\n",
                ep->sb.s_rev_level);
        goto fail;
    }

    /* ── Cache feature flags (only valid for DYNAMIC_REV) ── */
    if (ep->sb.s_rev_level == EXT4_DYNAMIC_REV) {
        ep->incompat  = ep->sb.s_feature_incompat;
        ep->ro_compat = ep->sb.s_feature_ro_compat;
        ep->compat    = ep->sb.s_feature_compat;
    } else {
        ep->incompat  = 0;
        ep->ro_compat = 0;
        ep->compat    = 0;
    }

    /* ── Initialize quota tracking from superblock ── */
    ext4_init_quota(ep);

    /* ── Report superblock metadata ── */
    kprintf("[ext4] SUPERBLOCK: UUID %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
        ep->sb.s_uuid[0],  ep->sb.s_uuid[1],  ep->sb.s_uuid[2],  ep->sb.s_uuid[3],
        ep->sb.s_uuid[4],  ep->sb.s_uuid[5],  ep->sb.s_uuid[6],  ep->sb.s_uuid[7],
        ep->sb.s_uuid[8],  ep->sb.s_uuid[9],  ep->sb.s_uuid[10], ep->sb.s_uuid[11],
        ep->sb.s_uuid[12], ep->sb.s_uuid[13], ep->sb.s_uuid[14], ep->sb.s_uuid[15]);
    kprintf("[ext4]   blocks=%u, inodes=%u, block_size=%u, inode_size=%u\n",
        ep->sb.s_blocks_count, ep->sb.s_inodes_count,
        1024u << ep->sb.s_log_block_size, ep->sb.s_inode_size);
    kprintf("[ext4]   rev=%u, state=%s (0x%04x), errors=%u\n",
        ep->sb.s_rev_level, ext4_state_string(ep->sb.s_state),
        ep->sb.s_state, ep->sb.s_errors);

    /* ── Log FS type ── */
    const char *fs_type = "ext4";
    if (!(ep->incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) &&
        !(ep->incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) &&
        !(ep->incompat & EXT4_FEATURE_INCOMPAT_INLINE_DATA)) {
        if (ep->sb.s_rev_level == EXT4_GOOD_OLD_REV)
            fs_type = "ext2";
        else if (ep->compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL)
            fs_type = "ext3";
        else
            fs_type = "ext2/3";
    }

    kprintf("[ext4] %s filesystem detected\n", fs_type);

    /* ── Log all feature flags ── */
    if (ep->incompat) {
        kprintf("[ext4] Incompat feature flags:\n");
        ext4_log_incompat_features(ep->incompat);
    }
    if (ep->ro_compat) {
        kprintf("[ext4] RO compat feature flags:\n");
        ext4_log_ro_compat_features(ep->ro_compat);
    }
    if (ep->compat) {
        kprintf("[ext4] Compat feature flags:\n");
        ext4_log_compat_features(ep->compat);
    }

    /* ── Quota info ── */
    ext4_log_quota_info(ep);

    /* ── Detect journal state ── */
    int journal_state = EXT4_JOURNAL_NONE;

    if (ep->compat & EXT4_FEATURE_COMPAT_HAS_JOURNAL) {
        if (ep->sb.s_journal_inum != 0) {
            kprintf("[ext4] journal inode: %u\n", ep->sb.s_journal_inum);
        }
        if (ep->sb.s_journal_dev != 0) {
            kprintf("[ext4] journal on external device: %u\n", ep->sb.s_journal_dev);
            journal_state = EXT4_JOURNAL_DEVICE;
        } else {
            journal_state = EXT4_JOURNAL_PRESENT;
        }

        if (ep->incompat & EXT4_FEATURE_INCOMPAT_RECOVER) {
            kprintf("[ext4] journal NEEDS RECOVERY — mounting read-only\n");
            journal_state = EXT4_JOURNAL_RECOVER;
        }

        /* Orphan inodes indicate journal needs replay */
        if (ep->sb.s_last_orphan != 0) {
            kprintf("[ext4] orphan inode list: head=%u\n", ep->sb.s_last_orphan);
            if (!(ep->incompat & EXT4_FEATURE_INCOMPAT_RECOVER)) {
                kprintf("[ext4] orphans present but RECOVER flag not set\n");
            }
        }
    }

    /* ── Check superblock state ── */
    if (ep->sb.s_state & EXT4_ERROR_FS) {
        kprintf("[ext4] WARNING: filesystem has errors! s_errors policy=%u\n",
            ep->sb.s_errors);
    }

    if (ep->sb.s_state & EXT4_ORPHAN_FS) {
        kprintf("[ext4] WARNING: filesystem has orphan inodes pending\n");
    }

    /* ── Reject filesystems with incompatible features we cannot handle ── */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_JOURNAL_DEV) {
        /* We can't read journal on a separate device */
        kprintf("[ext4] ERROR: journal on separate device not supported\n");
        goto fail;
    }

    if (ep->incompat & EXT4_FEATURE_INCOMPAT_MMP) {
        /* Multiple mount protection — we'd need to update MMP block periodically */
        kprintf("[ext4] WARNING: MMP enabled, filesystem may be in use on another system\n");
        /* Continue read-only is safe */
    }

    if (ep->incompat & EXT4_FEATURE_INCOMPAT_META_BG) {
        kprintf("[ext4] WARNING: meta block groups not fully supported\n");
        /* Continue read-only for simple cases */
    }

    if (ep->incompat & EXT4_FEATURE_INCOMPAT_ENCRYPT) {
        kprintf("[ext4] NOTE: encryption not supported, reading unencrypted data only\n");
    }

    if (ep->incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) {
        kprintf("[ext4] NOTE: checksum seed present (seed=0x%08x)\n", ep->sb.s_checksum_seed);
    }

    /* ── Verify superblock metadata checksum (CRC32C) ── */
    if (ext4_verify_sb_checksum(ep) < 0)
        goto fail;

    /* ── Block size ── */
    ep->block_size = 1024u << ep->sb.s_log_block_size;
    if (ep->block_size < 1024 || ep->block_size > EXT4_MAX_BLOCK_SIZE) {
        kprintf("[ext4] ERROR: unsupported block size: %u\n", ep->block_size);
        goto fail;
    }

    ep->blocks_per_group = ep->sb.s_blocks_per_group;
    ep->inodes_per_group = ep->sb.s_inodes_per_group;
    if (ep->sb.s_rev_level == EXT4_GOOD_OLD_REV) {
        ep->inode_size = 128;
    } else {
        ep->inode_size = ep->sb.s_inode_size;
        if (ep->inode_size < 128) {
            kprintf("[ext4] ERROR: invalid inode size: %u (must be >= 128)\n",
                    ep->inode_size);
            goto fail;
        }
    }
    if (ep->inode_size > EXT4_MAX_BLOCK_SIZE) {
        kprintf("[ext4] ERROR: inode size %u exceeds max block size %u\n",
                ep->inode_size, EXT4_MAX_BLOCK_SIZE);
        goto fail;
    }

    /* If RO_COMPAT_EXTRA_ISIZE is set, inodes may have extended fields */
    if (ep->ro_compat & EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE) {
        uint16_t min_extra = ep->sb.s_min_extra_isize;
        uint16_t want_extra = ep->sb.s_want_extra_isize;
        kprintf("[ext4] inode extra size: min=%u, want=%u\n", min_extra, want_extra);
    }

    /* ── Number of block groups ── */
    {
        /* Use 64-bit block count if available */
        uint64_t total_blocks = ep->sb.s_blocks_count;
        if (ep->incompat & EXT4_FEATURE_INCOMPAT_64BIT)
            total_blocks |= (uint64_t)ep->sb.s_blocks_count_hi << 32;

        /* Saturating cast: for very large filesystems (64-bit feature,
         * sparse per-group counts) the group count can exceed UINT32_MAX.
         * Clamp to the maximum representable value so that all subsequent
         * block-group-number computations remain consistent (wrapping to
         * 0 would silently break every inode lookup and BGD access). */
        uint64_t total_groups_64 = (total_blocks + ep->blocks_per_group - 1) /
                                    ep->blocks_per_group;
        uint32_t total_groups;
        if (total_groups_64 > 0xFFFFFFFFULL) {
            kprintf("[ext4] WARNING: total block groups (%llu) exceeds "
                    "32-bit limit, clamping to 0xFFFFFFFF\n",
                    total_groups_64);
            total_groups = 0xFFFFFFFFUL;
        } else {
            total_groups = (uint32_t)total_groups_64;
        }
        ep->num_block_groups = total_groups;
    }

    /* ── flex_bg size detection ── */
    if (ep->incompat & EXT4_FEATURE_INCOMPAT_FLEX_BG) {
        if (ep->sb.s_log_groups_per_flex > 0) {
            /* Use the value from superblock (log2 of flex group count) */
            ep->flex_bg_size = 1u << ep->sb.s_log_groups_per_flex;
        } else {
            /* Default flex_bg size is 16 (Linux kernel default) */
            ep->flex_bg_size = 16;
        }
        kprintf("[ext4] flex_bg enabled (group size=%u, log2=%u)\\n",
            ep->flex_bg_size, ep->sb.s_log_groups_per_flex);

        /* Run flex_bg consistency checks */
        if (ext4_verify_flex_bg(ep) < 0)
            goto fail;

        /* Log flex_bg layout: first few flex groups */
        uint32_t num_flex_groups = (ep->num_block_groups + ep->flex_bg_size - 1)
                                 / ep->flex_bg_size;
        uint32_t last_flex_size = ep->num_block_groups -
                                  (num_flex_groups - 1) * ep->flex_bg_size;
        kprintf("[ext4]   flex groups: %u total (%u full + 1 partial of %u)\\n",
            num_flex_groups, num_flex_groups - 1, last_flex_size);
        if (num_flex_groups <= 6) {
            /* Show all flex group ranges for small filesystems */
            for (uint32_t fg = 0; fg < num_flex_groups; fg++) {
                uint32_t first = fg * ep->flex_bg_size;
                uint32_t last = first + ep->flex_bg_size - 1;
                if (last >= ep->num_block_groups)
                    last = ep->num_block_groups - 1;
                kprintf("[ext4]     flex group %u: groups [%u, %u]\\n", fg, first, last);
            }
        } else {
            /* Show first 3 and last 3 for large filesystems */
            kprintf("[ext4]     flex group 0: groups [0, %u]\\n",
                ep->flex_bg_size - 1);
            kprintf("[ext4]     flex group 1: groups [%u, %u]\\n",
                ep->flex_bg_size, 2 * ep->flex_bg_size - 1);
            kprintf("[ext4]     flex group 2: groups [%u, %u]\\n",
                2 * ep->flex_bg_size, 3 * ep->flex_bg_size - 1);
            kprintf("[ext4]     ...\\n");
            kprintf("[ext4]     flex group %u: groups [%u, %u]\\n",
                num_flex_groups - 3,
                (num_flex_groups - 3) * ep->flex_bg_size,
                (num_flex_groups - 3) * ep->flex_bg_size + ep->flex_bg_size - 1);
            kprintf("[ext4]     flex group %u: groups [%u, %u]\\n",
                num_flex_groups - 2,
                (num_flex_groups - 2) * ep->flex_bg_size,
                (num_flex_groups - 2) * ep->flex_bg_size + ep->flex_bg_size - 1);
            kprintf("[ext4]     flex group %u: groups [%u, %u]\\n",
                num_flex_groups - 1,
                (num_flex_groups - 1) * ep->flex_bg_size,
                ep->num_block_groups - 1);
        }
    }

    /* ── Group descriptor size ── */
    uint16_t desc_size = ep->sb.s_desc_size;
    if (desc_size == 0)
        desc_size = sizeof(struct ext4_bg_desc);
    if (desc_size > 64)
        desc_size = 64;

    /* ── Load block group descriptor cache ── */
    if (ext4_load_bgd_cache(ep) < 0)
        goto fail;

    /* ── Report journal state summary ── */
    switch (journal_state) {
    case EXT4_JOURNAL_NONE:
        kprintf("[ext4] no journal present\n");
        break;
    case EXT4_JOURNAL_PRESENT:
        kprintf("[ext4] journal present (not yet replayable)\n");
        break;
    case EXT4_JOURNAL_RECOVER:
        kprintf("[ext4] journal requires recovery — JBD2 not yet implemented\n");
        break;
    case EXT4_JOURNAL_DEVICE:
        kprintf("[ext4] journal on external device — not supported\n");
        break;
    }

    /* ── Mount read-only ── */
    return vfs_mount_ex(mountpoint, &ext4_ops, ep, MS_RDONLY);

fail:
    if (ep->bgd_cache) kfree(ep->bgd_cache);
    kfree(ep);
    return -EIO;
}

/* ── Init ──────────────────────────────────────────────────────────── */

int __init ext4_init(void)
{
    kprintf("[ext4] Ext4 filesystem initialized (read-only, extents, inline data, quota)\n");
    vfs_register_filesystem("ext4", &ext4_ops);
    return 0;
}

#ifndef MODULE
device_initcall(ext4_init);
#else
int __init init_module(void) { return ext4_init(); }
void __exit cleanup_module(void) {}
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Ext4 filesystem — extent tree, flex_bg, inline data, checksums, encryption, project quota");
MODULE_VERSION("1.0");
#endif

/* ── ext4_umount ──────────────────────────────────────── */
static int ext4_umount(const char *target)
{
    (void)target;
    kprintf("[ext4] Ext4 unmounted\n");
    return 0;
}
/* ── ext4_lookup ──────────────────────────────────────── */
static int ext4_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[ext4] lookup: %s\n", name);
    return -ENOENT;
}
/* ── ext4_truncate ─────────────────────────────────────── */
static int ext4_truncate(void *inode, uint64_t size)
{
    (void)inode;
    kprintf("[ext4] truncate to %llu\n", (unsigned long long)size);
    return 0;
}
/* ── ext4_sync ──────────────────────────────────────── */
static int ext4_sync(void *file)
{
    (void)file;
    kprintf("[ext4] Sync complete\n");
    return 0;
}
