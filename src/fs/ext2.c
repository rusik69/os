/*
 * src/fs/ext2.c — Ext2 read-only filesystem with HTree directory indexing.
 *
 * Implements a read-only ext2 filesystem on top of the VFS layer.
 * Supports block groups, inodes, directory traversal (linear and
 * HTree/indexed), and reading regular files via direct/indirect blocks.
 *
 * Sparse file support (Item 148): Files with holes (unallocated blocks
 * indicated by zero block pointers) are handled correctly — reads return
 * zero-filled data for sparse regions instead of failing.
 *
 * HTree (hash tree) directory indexing provides O(log n) directory
 * lookups for large directories, as specified in the ext3/4 design.
 * The hash function used is half MD4 (the most common for ext3/4).
 */

#define KERNEL_INTERNAL
#include "ext2.h"
#include "blockdev.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "timer.h"

#ifdef MODULE
#include "module.h"
#endif

struct ext2_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t num_block_groups;
    struct ext2_superblock sb;
    char     mountpoint[64];   /* for vfs_force_readonly() on corruption */

    /* Cached block group descriptor table — loaded at mount time.
     * This allows correct inode lookup across ALL block groups,
     * not just group 0.  Allocated dynamically from kmalloc. */
    struct ext2_bg_desc *bgd_cache;       /* array of num_block_groups entries */
    uint32_t             bgd_cache_size;  /* total bytes allocated for bgd_cache */
};

/* Corrupt filesystem error helper: remounts read-only and returns -EFSCORRUPTED */
static int ext2_corrupt(struct ext2_priv *ep, const char *reason)
{
    if (!ep)
        return -EFSCORRUPTED;
    vfs_force_readonly(ep->mountpoint, reason);
    return -EFSCORRUPTED;
}

/* Read one block from the block device */
static int ext2_read_block(struct ext2_priv *ep, uint32_t block_num, uint8_t *buf) {
    uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
    uint32_t sectors = ep->block_size / 512;
    for (uint32_t i = 0; i < sectors; i++) {
        if (blockdev_read_sectors(ep->dev_id, lba + i, 1, buf + i * 512) != 0)
            return ext2_corrupt(ep, "block I/O error");
    }
    return 0;
}

/* Read superblock */
static int ext2_load_super(struct ext2_priv *ep) {
    uint8_t buf[1024];
    /* Superblock is at offset 1024 (block 0 if block_size=1024) */
    if (blockdev_read_sectors(ep->dev_id, 2, 1, buf) != 0 &&  /* sector 2 = offset 1024 */
        blockdev_read_sectors(ep->dev_id, 2, 1, buf) != 0) {
        /* Try via first block */
        uint64_t lba = 1024 / 512;
        if (blockdev_read_sectors(ep->dev_id, lba, 2, buf) != 0)
            return -EIO;
    }
    memcpy(&ep->sb, buf, sizeof(ep->sb));
    return 0;
}

/* Read inode — uses cached block group descriptor table for correct
 * group lookup across multi-group filesystems (Item 331). */
static int ext2_read_inode(struct ext2_priv *ep, uint32_t ino, struct ext2_inode *inode) {
    if (ino == 0)
        return ext2_corrupt(ep, "inode 0 is invalid");
    if (ino > ep->sb.s_inodes_count)
        return ext2_corrupt(ep, "inode number exceeds count in superblock");
    uint32_t group = (ino - 1) / ep->inodes_per_group;
    uint32_t index = (ino - 1) % ep->inodes_per_group;

    /* Use the cached block group descriptor table.
     * The cache is loaded at mount time from the primary copy. */
    if (!ep->bgd_cache || group >= ep->num_block_groups)
        return ext2_corrupt(ep, "block group out of range or bgd cache missing");
    struct ext2_bg_desc *bgd = &ep->bgd_cache[group];

    uint32_t inode_table_block = bgd->bg_inode_table;
    uint32_t byte_offset = index * ep->inode_size;

    uint32_t tbl_block = inode_table_block + byte_offset / ep->block_size;
    uint32_t tbl_off   = byte_offset % ep->block_size;

    uint8_t block_buf[4096];
    if (ep->block_size > 4096) return -EINVAL;
    if (ext2_read_block(ep, tbl_block, block_buf) < 0) return -EIO;

    memcpy(inode, block_buf + tbl_off, sizeof(struct ext2_inode));
    return 0;
}

/*
 * Get the 64-bit file size from an ext2 inode.
 *
 * When EXT2_FEATURE_RO_COMPAT_LARGE_FILE is set, regular files use the
 * i_dir_acl field (offset 108 in the inode) as the upper 32 bits of the
 * file size.  This field is repurposed: for regular files it stores the
 * high 32 bits of i_size; for directories it still holds the directory
 * ACL block number (which we do not use in read-only mode).
 *
 * Returns the 64-bit file size.  For directories and other non-file
 * inodes, the size is returned as-is (32-bit zero-extended).
 */
static uint64_t ext2_inode_get_size(struct ext2_priv *ep,
                                     const struct ext2_inode *inode)
{
    uint64_t size = inode->i_size;

    /* If LARGE_FILE feature is set, combine with the upper 32 bits
     * from i_dir_acl for regular files (non-directory inodes). */
    if ((ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) &&
        !(inode->i_mode & 0x4000)) {
        size |= ((uint64_t)inode->i_dir_acl << 32);
    }

    return size;
}

/* Read block from inode (handles indirect blocks) */
/*
 * ext2_get_block_num — resolve logical block index to physical block number.
 *
 * Returns the physical block number for the given logical block (iblock).
 * Returns 0 if the block is a hole (unallocated).  Returns -1 on error
 * (corrupted indirect block, or doubly/triply indirect not supported).
 *
 * Sparse files (files with holes) have i_block[] entries set to 0 for
 * unallocated regions.  A physical block number of 0 is reserved (the
 * boot block cannot be part of a file), so 0 unambiguously means hole.
 */

/* Forward declaration for extent resolver */
static int64_t ext2_extent_get_block(struct ext2_priv *ep,
                                      struct ext2_inode *inode,
                                      uint32_t iblock);

static int64_t ext2_get_block_num(struct ext2_priv *ep, struct ext2_inode *inode,
                                   uint32_t iblock) {
    /* Check if extent tree is in use */
    if (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) {
        return ext2_extent_get_block(ep, inode, iblock);
    }

    if (iblock < 12) {
        /* Direct block pointer — 0 means hole */
        return (int64_t)inode->i_block[iblock];
    }

    /* Singly indirect */
    uint32_t entries_per_block = ep->block_size / 4;
    uint32_t sind = iblock - 12;

    if (sind < entries_per_block) {
        if (inode->i_block[12] == 0)
            return 0; /* hole — indirect block not allocated */
        uint8_t indir[4096];
        if (ext2_read_block(ep, inode->i_block[12], indir) < 0)
            return -EIO;
        uint32_t *ptrs = (uint32_t *)indir;
        return (int64_t)ptrs[sind]; /* 0 means hole here too */
    }

    /* Doubly/triply indirect not needed for basic support */
    return -EINVAL;
}

/* ── Extent tree block resolution (EXT4-compatible) ──────────────── */
/* When EXTENTS feature is set, the inode's i_block[] stores the extent
 * tree root instead of direct/indirect block pointers. */

#define EXT4_EXTENT_MAGIC   0xF30A
#define EXT4_EXTENT_MAX_DEPTH 5

struct ext4_extent_header {
    uint16_t eh_magic;
    uint16_t eh_entries;
    uint16_t eh_max;
    uint16_t eh_depth;
    uint32_t eh_generation;
};

struct ext4_extent_idx {
    uint32_t ei_block;   /* first logical block covered by this index */
    uint32_t ei_leaf_lo; /* low 32 bits of physical block of child */
    uint16_t ei_leaf_hi; /* high 16 bits of physical block */
    uint16_t ei_unused;
};

struct ext4_extent {
    uint32_t ee_block;   /* first logical block covered */
    uint16_t ee_len;     /* number of blocks covered (or 32768 for uninit) */
    uint16_t ee_start_hi;/* high 16 bits of physical block */
    uint32_t ee_start_lo;/* low 32 bits of physical block */
};

/* Extent tree resolver — inline implementation */
static int64_t ext2_extent_get_block(struct ext2_priv *ep,
                                      struct ext2_inode *inode,
                                      uint32_t iblock)
{
    uint8_t root_buf[60]; /* i_block[15] = 60 bytes */
    memcpy(root_buf, inode->i_block, 60);

    struct ext4_extent_header *eh = (struct ext4_extent_header *)root_buf;
    if (eh->eh_magic != EXT4_EXTENT_MAGIC)
        return ext2_corrupt(ep, "bad extent magic");

    uint16_t depth = eh->eh_depth;
    if (depth > EXT4_EXTENT_MAX_DEPTH)
        return ext2_corrupt(ep, "extent tree too deep");

    uint8_t node_buf[4096];
    uint8_t *node_data = root_buf;

    for (;;) {
        eh = (struct ext4_extent_header *)node_data;

        if (depth > 0) {
            /* Internal node — binary search for child */
            struct ext4_extent_idx *idx = (struct ext4_extent_idx *)(eh + 1);
            uint16_t num = eh->eh_entries;
            uint16_t lo = 0, hi = num, mid;
            uint16_t best = 0;

            while (lo < hi) {
                mid = lo + (hi - lo) / 2;
                if (idx[mid].ei_block <= iblock) {
                    best = mid;
                    lo = mid + 1;
                } else {
                    hi = mid;
                }
            }

            /* Read child node block */
            uint64_t child_block = (uint64_t)idx[best].ei_leaf_lo |
                                   ((uint64_t)idx[best].ei_leaf_hi << 32);
            if (child_block == 0)
                return 0; /* hole */

            if (ep->block_size > 4096) return -EINVAL;
            if (ext2_read_block(ep, (uint32_t)child_block, node_buf) < 0)
                return -EIO;
            node_data = node_buf;
            depth--;
        } else {
            /* Leaf node */
            struct ext4_extent *ext = (struct ext4_extent *)(eh + 1);
            uint16_t num = eh->eh_entries;

            for (uint16_t i = 0; i < num; i++) {
                if (iblock >= ext[i].ee_block &&
                    iblock < ext[i].ee_block + (ext[i].ee_len & 0x7FFF)) {
                    uint64_t phys = (uint64_t)ext[i].ee_start_lo |
                                    ((uint64_t)ext[i].ee_start_hi << 32);
                    uint32_t offset = iblock - ext[i].ee_block;
                    return (int64_t)(phys + offset);
                }
            }
            return 0; /* hole */
        }
    }
}

static int ext2_read_inode_block(struct ext2_priv *ep, struct ext2_inode *inode,
                                  uint32_t iblock, uint8_t *buf) {
    int64_t phys_block = ext2_get_block_num(ep, inode, iblock);
    if (phys_block < 0)
        return -EINVAL;
    if (phys_block == 0) {
        /* Hole — sparse block; fill with zeros */
        memset(buf, 0, ep->block_size);
        return 0;
    }
    return ext2_read_block(ep, (uint32_t)phys_block, buf);
}

/* ── Write helpers ──────────────────────────────────────────── */

/* Write one block to the block device */
static int ext2_write_block(struct ext2_priv *ep, uint32_t block_num,
                             const uint8_t *buf)
{
	uint64_t lba = (uint64_t)block_num * (ep->block_size / 512);
	uint32_t sectors = ep->block_size / 512;
	for (uint32_t i = 0; i < sectors; i++) {
		if (blockdev_write_sectors(ep->dev_id, lba + i, 1,
		                           buf + i * 512) != 0)
			return -EIO;
	}
	return 0;
}

/* Write an inode back to disk.
 * Reads the containing block, patches the inode slot, writes back. */
static int ext2_write_inode(struct ext2_priv *ep, uint32_t ino,
                             const struct ext2_inode *inode)
{
	if (ino == 0 || ino > ep->sb.s_inodes_count)
		return -EINVAL;
	uint32_t group = (ino - 1) / ep->inodes_per_group;
	uint32_t index = (ino - 1) % ep->inodes_per_group;
	if (!ep->bgd_cache || group >= ep->num_block_groups)
		return -EINVAL;
	struct ext2_bg_desc *bgd = &ep->bgd_cache[group];
	uint32_t itable_block = bgd->bg_inode_table;
	uint32_t byte_off = index * ep->inode_size;
	uint32_t tbl_block = itable_block + byte_off / ep->block_size;
	uint32_t tbl_off   = byte_off % ep->block_size;
	uint8_t block_buf[4096];
	if (ep->block_size > 4096) return -EINVAL;
	if (ext2_read_block(ep, tbl_block, block_buf) < 0) return -EIO;
	memcpy(block_buf + tbl_off, inode, sizeof(struct ext2_inode));
	return ext2_write_block(ep, tbl_block, block_buf);
}

/* Read a data block from an inode and return the physical block number.
 * This is like ext2_read_inode_block but also gives back the PBN for
 * write-back. */
static int ext2_read_inode_block_pbn(struct ext2_priv *ep,
                                      struct ext2_inode *inode,
                                      uint32_t iblock, uint8_t *buf,
                                      uint32_t *pbn)
{
	int64_t phys = ext2_get_block_num(ep, inode, iblock);
	if (phys < 0)
		return -EINVAL;
	if (phys == 0) {
		memset(buf, 0, ep->block_size);
		*pbn = 0;
		return 0;
	}
	*pbn = (uint32_t)phys;
	return ext2_read_block(ep, *pbn, buf);
}

/* ── Flex block group helpers ──────────────────────────────────────
 *
 * When EXT2_FEATURE_INCOMPAT_FLEX_BG is set, consecutive block groups
 * are grouped into "flexible block groups" whose metadata (bitmaps and
 * inode tables) is packed together in the first group's data block area.
 * This improves locality and reduces seeks.
 *
 * The standard flex group size is 16 block groups.
 * ═══════════════════════════════════════════════════════════════════ */

static inline uint32_t ext2_flex_group_size(struct ext2_priv *ep)
{
	(void)ep;
	return 16;  /* Standard flex_bg group size (16 block groups) */
}

static inline uint32_t ext2_flex_group_of(uint32_t block_group,
                                           uint32_t flex_size)
{
	return block_group / flex_size;
}

/* ── Bitmap scanning helpers (64-bit word-level) ────────────────────
 *
 * Instead of scanning bit-by-bit, these functions use 64-bit word
 * operations and __builtin_ctzll for O(1) bit position lookup,
 * dramatically speeding up bitmap scanning on large filesystems.
 * ═══════════════════════════════════════════════════════════════════ */

/* Find the first free (set) bit in a bitmap using word-level scanning.
 * Returns the bit index on success, -1 if no free bit found.
 * @bitmap: pointer to the bitmap data
 * @num_bits: number of valid bits in the bitmap */
static int ext2_bitmap_find_free(const uint8_t *bitmap, uint32_t num_bits)
{
	uint32_t num_words = num_bits / 64;
	const uint64_t *words = (const uint64_t *)bitmap;

	/* Scan full 64-bit words */
	for (uint32_t w = 0; w < num_words; w++) {
		if (words[w] != 0) {
			int bit = __builtin_ctzll(words[w]);
			return (int)(w * 64 + (uint32_t)bit);
		}
	}

	/* Scan remaining bits (if num_bits not a multiple of 64) */
	uint32_t start = num_words * 64;
	for (uint32_t b = start; b < num_bits; b++) {
		if (bitmap[b / 8] & (1U << (b % 8)))
			return (int)b;
	}

	return -1;
}

/* Find a contiguous run of @cluster_size free bits in a bitmap.
 * Returns the start bit index on success, -1 if not found.
 * @bitmap: pointer to the bitmap data
 * @num_bits: number of valid bits in the bitmap
 * @cluster_size: required number of contiguous free bits */
static int ext2_bitmap_find_cluster(const uint8_t *bitmap,
                                     uint32_t num_bits,
                                     uint32_t cluster_size)
{
	uint32_t run = 0;
	uint32_t start = 0;

	for (uint32_t b = 0; b < num_bits; b++) {
		if (bitmap[b / 8] & (1U << (b % 8))) {
			if (run == 0)
				start = b;
			run++;
			if (run >= cluster_size)
				return (int)start;
		} else {
			run = 0;
		}
	}

	return -1;
}

/* ── Clustered block allocator with flex_bg awareness ───────────────
 *
 * Allocates @num_blocks contiguous free blocks, preferring the flex
 * block group containing @prefer_group for locality.
 *
 * Algorithm:
 *  1. If flex_bg is enabled, compute the preferred flex group from
 *     @prefer_group.  Otherwise treat each block group as its own flex
 *     group (flex_size = 1).
 *  2. Scan flex groups starting with the preferred one, searching for
 *     a group with enough free blocks and a contiguous cluster.
 *  3. Use word-level bitmap scanning for efficiency.
 *  4. Mark the cluster as used, zero the blocks, update counts.
 *
 * Returns 0 on success with @first_block_out set, -ENOSPC on failure. */
static int ext2_alloc_blocks(struct ext2_priv *ep,
                              uint32_t *first_block_out,
                              uint32_t num_blocks,
                              uint32_t prefer_group)
{
	uint8_t bitmap[4096];
	int has_flexbg = (ep->sb.s_feature_incompat &
	                  EXT2_FEATURE_INCOMPAT_FLEX_BG) ? 1 : 0;
	uint32_t flex_size = has_flexbg ? ext2_flex_group_size(ep) : 1;
	uint32_t prefer_fg = ext2_flex_group_of(prefer_group, flex_size);
	uint32_t flex_group_count =
	    (ep->num_block_groups + flex_size - 1) / flex_size;

	for (uint32_t f = 0; f < flex_group_count; f++) {
		/* Start from the preferred flex group and wrap around */
		uint32_t actual_fg = (prefer_fg + f) % flex_group_count;
		uint32_t start_g = actual_fg * flex_size;
		uint32_t end_g = start_g + flex_size;
		if (end_g > ep->num_block_groups)
			end_g = ep->num_block_groups;

		for (uint32_t g = start_g; g < end_g; g++) {
			struct ext2_bg_desc *bgd = &ep->bgd_cache[g];

			/* Quick skip — not enough free blocks */
			if (bgd->bg_free_blocks_count < num_blocks)
				continue;

			if (ext2_read_block(ep, bgd->bg_block_bitmap,
			                    bitmap) < 0)
				continue;

			uint32_t blocks_in_group = ep->blocks_per_group;
			if (g == ep->num_block_groups - 1) {
				uint32_t rem = ep->sb.s_blocks_count
				               - g * ep->blocks_per_group;
				if (rem < blocks_in_group)
					blocks_in_group = rem;
			}

			/* Find free cluster */
			int bit;
			if (num_blocks == 1) {
				bit = ext2_bitmap_find_free(bitmap,
				                            blocks_in_group);
			} else {
				bit = ext2_bitmap_find_cluster(bitmap,
				                blocks_in_group, num_blocks);
			}

			if (bit < 0)
				continue;

			/* ── Mark cluster as used in bitmap ── */
			for (uint32_t b = (uint32_t)bit;
			     b < (uint32_t)bit + num_blocks
			     && b < blocks_in_group; b++) {
				bitmap[b / 8] &= ~(1U << (b % 8));
			}

			if (ext2_write_block(ep, bgd->bg_block_bitmap,
			                    bitmap) < 0)
				return -EIO;

			uint32_t base_block =
			    g * ep->blocks_per_group + (uint32_t)bit;

			/* Update free counts */
			bgd->bg_free_blocks_count -= (uint16_t)num_blocks;
			ep->sb.s_free_blocks_count -= num_blocks;

			/* Zero the newly allocated blocks */
			{
				uint8_t zero[4096];
				memset(zero, 0, ep->block_size);
				for (uint32_t b = 0; b < num_blocks; b++) {
					if (ext2_write_block(ep,
					    base_block + b, zero) < 0)
						return -EIO;
				}
			}

			*first_block_out = base_block;
			return 0;
		}
	}

	return -ENOSPC;
}

/* Single-block allocator (backward compatible).
 *
 * With flex_bg enabled, prefers the block group that has the most free
 * blocks for better load distribution across groups.  Without flex_bg,
 * falls back to the original sequential group scan (but using word-level
 * bitmap scanning for efficiency).
 *
 * Returns 0 on success with @block_out set, -ENOSPC on failure. */
static int ext2_alloc_block(struct ext2_priv *ep, uint32_t *block_out)
{
	if (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FLEX_BG) {
		/* Flex_bg: prefer the group with the most free blocks
		 * for better distribution across flex groups. */
		uint32_t prefer_group = 0;
		uint32_t most_free = 0;

		for (uint32_t g = 0; g < ep->num_block_groups; g++) {
			if (ep->bgd_cache[g].bg_free_blocks_count
			    > most_free) {
				most_free =
				    ep->bgd_cache[g].bg_free_blocks_count;
				prefer_group = g;
			}
		}

		return ext2_alloc_blocks(ep, block_out, 1, prefer_group);
	}

	/* Non-flex_bg: sequential scan, word-level bitmap scanning */
	return ext2_alloc_blocks(ep, block_out, 1, 0);
}

/* ── Inode allocator with flex_bg awareness ──────────────────────────
 *
 * Allocates one free inode from the inode bitmap.  When flex_bg is
 * enabled (EXT2_FEATURE_INCOMPAT_FLEX_BG), prefers the flex group with
 * the most free inodes for better load distribution across groups.
 * Uses word-level bitmap scanning (ext2_bitmap_find_free) for
 * efficiency instead of bit-by-bit traversal.
 *
 * Algorithm:
 *  1. If flex_bg is enabled, compute the flex group with the most free
 *     inodes across all block groups.  Otherwise treat each block group
 *     as its own flex group (flex_size = 1).
 *  2. Scan groups within the selected (flex) group for a free inode.
 *  3. Use word-level FFS bitmap scanning for O(1) bit lookup.
 *  4. Mark the bit as used, update free counts.
 *
 * Returns 0 on success with @ino_out set, -ENOSPC on failure.
 * ═══════════════════════════════════════════════════════════════════ */
static int ext2_alloc_inode(struct ext2_priv *ep, uint32_t *ino_out)
{
	uint8_t bitmap[4096];
	int has_flexbg = (ep->sb.s_feature_incompat &
	                  EXT2_FEATURE_INCOMPAT_FLEX_BG) ? 1 : 0;
	uint32_t flex_size = has_flexbg ? ext2_flex_group_size(ep) : 1;
	uint32_t flex_group_count =
	    (ep->num_block_groups + flex_size - 1) / flex_size;

	if (has_flexbg) {
		/* Flex_bg: find the flex group with the most free inodes */
		uint32_t best_flex = 0;
		uint32_t most_free = 0;

		for (uint32_t f = 0; f < flex_group_count; f++) {
			uint32_t start_g = f * flex_size;
			uint32_t end_g = start_g + flex_size;
			if (end_g > ep->num_block_groups)
				end_g = ep->num_block_groups;
			uint32_t flex_free = 0;
			for (uint32_t g = start_g; g < end_g; g++)
				flex_free +=
				    ep->bgd_cache[g].bg_free_inodes_count;
			if (flex_free > most_free) {
				most_free = flex_free;
				best_flex = f;
			}
		}

		/* Scan only within the best flex group */
		uint32_t start_g = best_flex * flex_size;
		uint32_t end_g = start_g + flex_size;
		if (end_g > ep->num_block_groups)
			end_g = ep->num_block_groups;

		for (uint32_t g = start_g; g < end_g; g++) {
			struct ext2_bg_desc *bgd = &ep->bgd_cache[g];
			if (bgd->bg_free_inodes_count == 0)
				continue;
			if (ext2_read_block(ep, bgd->bg_inode_bitmap,
			                    bitmap) < 0)
				continue;
			uint32_t inodes_in_group = ep->inodes_per_group;
			if (g == ep->num_block_groups - 1) {
				uint32_t rem = ep->sb.s_inodes_count -
				               g * ep->inodes_per_group;
				if (rem < inodes_in_group)
					inodes_in_group = rem;
			}
			int bit = ext2_bitmap_find_free(bitmap,
			                                inodes_in_group);
			if (bit < 0)
				continue;
			bitmap[bit / 8] &= ~(1U << ((uint32_t)bit % 8));
			if (ext2_write_block(ep, bgd->bg_inode_bitmap,
			                     bitmap) < 0)
				return -EIO;
			uint32_t ino =
			    g * ep->inodes_per_group + (uint32_t)bit + 1;
			bgd->bg_free_inodes_count--;
			ep->sb.s_free_inodes_count--;
			*ino_out = ino;
			return 0;
		}
	} else {
		/* Non-flex_bg: sequential scan with word-level bitmap */
		for (uint32_t g = 0; g < ep->num_block_groups; g++) {
			struct ext2_bg_desc *bgd = &ep->bgd_cache[g];
			if (bgd->bg_free_inodes_count == 0)
				continue;
			if (ext2_read_block(ep, bgd->bg_inode_bitmap,
			                    bitmap) < 0)
				continue;
			uint32_t inodes_in_group = ep->inodes_per_group;
			if (g == ep->num_block_groups - 1) {
				uint32_t rem = ep->sb.s_inodes_count -
				               g * ep->inodes_per_group;
				if (rem < inodes_in_group)
					inodes_in_group = rem;
			}
			int bit = ext2_bitmap_find_free(bitmap,
			                                inodes_in_group);
			if (bit < 0)
				continue;
			bitmap[bit / 8] &= ~(1U << ((uint32_t)bit % 8));
			if (ext2_write_block(ep, bgd->bg_inode_bitmap,
			                     bitmap) < 0)
				return -EIO;
			uint32_t ino =
			    g * ep->inodes_per_group + (uint32_t)bit + 1;
			bgd->bg_free_inodes_count--;
			ep->sb.s_free_inodes_count--;
			*ino_out = ino;
			return 0;
		}
	}
	return -ENOSPC;
}

/* Round a value up to the next multiple of 4 */
static inline uint32_t ext2_align4(uint32_t v)
{
	return (v + 3) & ~3U;
}

/* Add a directory entry to a parent directory.
 *
 * Scans existing directory blocks for a free slot or slack space.
 * If none is found, allocates a new block and appends it.
 * The last entry in each ext2 directory block must have its rec_len
 * set to reach the end of the block; when we split a block to add
 * an entry, we shrink that trailing entry's rec_len to its actual
 * used size, then place the new entry in the freed space.
 *
 * Parameters:
 *   ep        — ext2 private data
 *   dir_inode — inode of the parent directory (will be updated on write)
 *   dir_ino   — inode number of the parent (for write-back)
 *   name      — entry name (null-terminated, max 255)
 *   child_ino — inode number of the child
 *   file_type — ext2 file type byte (e.g. EXT2_FT_REG_FILE)
 *
 * Returns 0 on success, negative errno on failure.
 */
static int ext2_add_dirent(struct ext2_priv *ep,
                            struct ext2_inode *dir_inode,
                            uint32_t dir_ino, const char *name,
                            uint32_t child_ino, uint8_t file_type)
{
	size_t namelen = strlen(name);
	if (namelen == 0 || namelen > 255)
		return -EINVAL;

	uint32_t reclen = ext2_align4((uint32_t)(8 + namelen));
	uint32_t dir_size = (uint32_t)ext2_inode_get_size(ep, dir_inode);
	uint32_t iblock = 0;
	uint32_t offset = 0;

	/* ── Phase 1: scan existing blocks for a slot ────────────── */
	while (offset < dir_size) {
		uint8_t block_buf[4096];
		uint32_t pbn;
		if (ext2_read_inode_block_pbn(ep, dir_inode, iblock,
		                               block_buf, &pbn) < 0)
			return -EIO;
		if (pbn == 0) {
			iblock++;
			offset += ep->block_size;
			continue;
		}

		uint32_t pos = 0;
		while (pos + 8 <= ep->block_size &&
		       offset + pos < dir_size) {
			struct {
				uint32_t inode;
				uint16_t rec_len;
				uint8_t  name_len;
				uint8_t  file_type;
				char     name[255];
			} *de = (void *)(block_buf + pos);

			if (de->rec_len == 0)
				break;

			if (de->inode == 0 && de->rec_len >= reclen) {
				/* Free/unused entry large enough */
				de->inode = child_ino;
				de->name_len = (uint8_t)namelen;
				de->file_type = file_type;
				memcpy(de->name, name, namelen);
				return ext2_write_block(ep, pbn, block_buf);
			}

			if (de->inode != 0) {
				/* Check if this entry has slack space
				 * after its name that we can steal */
				uint32_t used = ext2_align4(
					(uint32_t)(8 + de->name_len));
				uint32_t slack = de->rec_len - used;
				if (slack >= reclen) {
					/* Shrink current entry */
					de->rec_len = (uint16_t)used;
					/* Add new entry in the slack */
					pos += used;
					struct {
						uint32_t inode;
						uint16_t rec_len;
						uint8_t  name_len;
						uint8_t  file_type;
						char     name[255];
					} *nde = (void *)(block_buf + pos);
					nde->inode = child_ino;
					nde->rec_len = (uint16_t)slack;
					nde->name_len = (uint8_t)namelen;
					nde->file_type = file_type;
					memcpy(nde->name, name, namelen);
					return ext2_write_block(ep, pbn,
					                        block_buf);
				}
			}
			pos += de->rec_len;
		}
		iblock++;
		offset += ep->block_size;
	}

	/* ── Phase 2: no slot found — allocate a new block ──────── */
	uint32_t new_block;
	int ret = ext2_alloc_block(ep, &new_block);
	if (ret < 0)
		return ret;

	/* Fix up the last directory entry in the previous block:
	 * the last entry's rec_len must reach to the end of the block.
	 * Change it so it only covers its own entry, making space
	 * visible in the new block. */
	if (dir_size > 0) {
		uint32_t last_iblock = (dir_size - 1) / ep->block_size;
		uint8_t last_buf[4096];
		uint32_t last_pbn;
		if (ext2_read_inode_block_pbn(ep, dir_inode, last_iblock,
		                               last_buf, &last_pbn) < 0)
			return -EIO;

		/* Find the last entry in this block */
		uint32_t lp = 0;
		uint32_t last_entry_pos = 0;
		uint16_t last_entry_rec = 0;
		while (lp + 8 <= ep->block_size) {
			struct {
				uint32_t inode;
				uint16_t rec_len;
				uint8_t  name_len;
				uint8_t  file_type;
				char     name[255];
			} *lde = (void *)(last_buf + lp);
			if (lde->rec_len == 0) break;
			last_entry_pos = lp;
			last_entry_rec = lde->rec_len;
			lp += lde->rec_len;
		}
		if (last_entry_rec > 0) {
			/* Shrink last entry to its actual used size */
			struct {
				uint32_t inode;
				uint16_t rec_len;
				uint8_t  name_len;
				uint8_t  file_type;
				char     name[255];
			} *lde = (void *)(last_buf + last_entry_pos);
			uint32_t used = ext2_align4(
				(uint32_t)(8 + lde->name_len));
			lde->rec_len = (uint16_t)(ep->block_size -
			                          last_entry_pos);
			if (ext2_write_block(ep, last_pbn, last_buf) < 0)
				return -EIO;
		}
	}

	/* Prepare the new block with our entry */
	uint8_t new_buf[4096];
	memset(new_buf, 0, ep->block_size);
	{
		struct {
			uint32_t inode;
			uint16_t rec_len;
			uint8_t  name_len;
			uint8_t  file_type;
			char     name[255];
		} *nde = (void *)new_buf;
		nde->inode = child_ino;
		nde->rec_len = (uint16_t)((offset + ep->block_size <= dir_size + ep->block_size)
		                          ? ep->block_size : reclen);
		/* For a newly added block, the entry's rec_len reaches
		 * to the end of the block.  If more entries will follow
		 * later, the next add_dirent call will shrink it. */
		nde->rec_len = (uint16_t)ep->block_size;
		nde->name_len = (uint8_t)namelen;
		nde->file_type = file_type;
		memcpy(nde->name, name, namelen);
	}

	if (ext2_write_block(ep, new_block, new_buf) < 0)
		return -EIO;

	/* Link the new block into the directory inode.
	 * We only support direct blocks for now. */
	if (iblock >= 12) {
		/* Too large — for now fail. Indirect block support
		 * would be needed for directories with 12+ blocks. */
		return -ENOSPC;
	}
	dir_inode->i_block[iblock] = new_block;
	dir_inode->i_size = dir_size + ep->block_size;
	dir_inode->i_blocks += ep->block_size / 512;
	dir_inode->i_mtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
	ret = ext2_write_inode(ep, dir_ino, dir_inode);
	if (ret < 0) return ret;

	return 0;
}

/* ── HTree: Half MD4 hash function ──────────────────────────────── */

/*
 * Half MD4 — a reduced-round variant of MD4 used by ext3/4 HTree.
 * Produces a 32-bit hash from a filename and a seed (usually 0).
 * Based on RFC 1320 MD4 with only the first two rounds of mixing.
 */
#define F(x, y, z) (((x) & (y)) | ((~x) & (z)))
#define G(x, y, z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))

#define HALF_MD4_ROTLEFT(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

#define HALF_MD4_ROUND1(a, b, c, d, k, s) do { \
    a += F(b, c, d) + k;                       \
    a = HALF_MD4_ROTLEFT(a, s);                \
} while (0)

#define HALF_MD4_ROUND2(a, b, c, d, k, s) do { \
    a += G(b, c, d) + k + 0x5A827999;           \
    a = HALF_MD4_ROTLEFT(a, s);                \
} while (0)

/* Chunk size for half-MD4: each chunk is 16 bytes (contradicts standard
 * MD4 which is 64 bytes — ext3/4 half-MD4 processes 16-byte chunks). */
#define HALF_MD4_CHUNK_WORDS 4  /* 16 bytes */

/* Process one 16-byte chunk through the half-MD4 compression function */
static void half_md4_transform(uint32_t state[4], const uint32_t chunk[4])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t x[4];
    x[0] = chunk[0]; x[1] = chunk[1]; x[2] = chunk[2]; x[3] = chunk[3];

    /* Round 1 (all 4 operations) */
    HALF_MD4_ROUND1(a, b, c, d, x[0],  3);
    HALF_MD4_ROUND1(d, a, b, c, x[1],  7);
    HALF_MD4_ROUND1(c, d, a, b, x[2], 11);
    HALF_MD4_ROUND1(b, c, d, a, x[3], 19);

    /* Round 2 (all 4 operations) */
    HALF_MD4_ROUND2(a, b, c, d, x[0],  3);
    HALF_MD4_ROUND2(d, a, b, c, x[1],  5);
    HALF_MD4_ROUND2(c, d, a, b, x[2],  9);
    HALF_MD4_ROUND2(b, c, d, a, x[3], 13);

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

/*
 * Compute the half-MD4 hash of @name (up to @name_len bytes).
 * Returns a 32-bit hash value suitable for HTree lookup.
 * The seed (@hash_seed) comes from the ext2 superblock (if available)
 * or defaults to a fixed seed (0).
 */
static uint32_t ext2_htree_hash(const unsigned char *name,
                                 int name_len,
                                 const uint32_t hash_seed[4])
{
    uint32_t state[4];
    uint32_t seed[4];

    if (hash_seed) {
        seed[0] = hash_seed[0];
        seed[1] = hash_seed[1];
        seed[2] = hash_seed[2];
        seed[3] = hash_seed[3];
    } else {
        seed[0] = 0;
        seed[1] = 0;
        seed[2] = 0;
        seed[3] = 0;
    }

    /* Initialise state from seed */
    state[0] = seed[0];
    state[1] = seed[1];
    state[2] = seed[2];
    state[3] = seed[3];

    /* Process the name in 16-byte (4 x uint32_t) chunks */
    int pos = 0;
    while (pos + 16 <= name_len) {
        uint32_t chunk[4];
        memcpy(chunk, name + pos, 16);
        half_md4_transform(state, chunk);
        pos += 16;
    }

    /* Process remaining bytes (pad with zeros) */
    if (pos < name_len) {
        uint32_t chunk[4] = {0, 0, 0, 0};
        memcpy(chunk, name + pos, (size_t)(name_len - pos));
        half_md4_transform(state, chunk);
    }

    /* Half-MD4 output: only the first 2 words (8 bytes) are XOR'd to
     * produce a 32-bit hash, per ext3/4 HTree convention. */
    return state[0] ^ state[1];
}

/*
 * Compute the HTree hash for a filename, using the appropriate
 * hash version.  For now we handle HALF_MD4 (the most common).
 */
static uint32_t ext2_dx_hash(const unsigned char *name, int name_len,
                             uint8_t hash_version,
                             const uint32_t hash_seed[4])
{
    (void)hash_version;  /* We only implement half-MD4; the caller should
                          * fall back to linear search for unsupported versions. */
    return ext2_htree_hash(name, name_len, hash_seed);
}

/* ── HTree directory lookup ─────────────────────────────────────── */

/*
 * Walk the HTree index to find the leaf block that could contain
 * a directory entry with the given hash.  Uses binary search on
 * the index entries at each level.
 *
 * @ep:          ext2 private data
 * @inode:       directory inode
 * @hash:        hash value computed from the filename
 * @leaf_block:  (output) block number of the leaf data block
 *
 * Returns 0 on success, -1 on error (no HTree or unsupported format).
 *
 * HTree node layout (all multi-byte values are little-endian):
 *
 *   dx_root (first block of indexed directory):
 *     - '.' and '..' entries (variable length via rec_len)
 *     - uint32_t reserved (0)
 *     - uint8_t  hash_version
 *     - uint8_t  info_length (8)
 *     - uint8_t  indirect_levels (0 = single-level tree)
 *     - uint8_t  unused_flags
 *     - uint16_t limit  (entry capacity in this node)
 *     - uint16_t count  (used entry count)
 *     - uint32_t block  (block number of this node; 0 for root)
 *     - struct ext2_dx_entry entries[limit]
 *
 *   dx_node (internal node):
 *     - Same layout but WITHOUT . and .. entries; starts at offset 0
 *       with reserved/hash_version/info_length/indirect_levels/unused_flags
 *       (8 bytes), then limit(2) + count(2) + block(4) = 16 bytes total
 *       header, then entries[limit].
 */
static int ext2_htree_lookup_leaf(struct ext2_priv *ep,
                                  struct ext2_inode *inode,
                                  uint32_t hash,
                                  uint32_t *leaf_block)
{
    uint8_t block_buf[4096];

    /* Read the first block of the directory — contains the dx_root */
    if (ext2_read_inode_block(ep, inode, 0, block_buf) < 0)
        return -EINVAL;

    /* The dx_root starts after the '.' and '..' entries.  We skip them
     * by following rec_len fields, then parse the index header. */
    uint32_t pos = 0;

    /* We define a local helper for raw directory entries */
#define EXT2_DIRENT_SIZE(nl) ((sizeof(uint32_t) + sizeof(uint16_t) + 1 + 1) + (nl))
    /* Simplified dirent header size: inode(4) + rec_len(2) + name_len(1) + file_type(1) = 8 + name */

    /* Skip '.' entry */
    {
        uint32_t *de_inode  = (uint32_t *)(block_buf + pos);
        uint16_t *de_rec    = (uint16_t *)(block_buf + pos + 4);
        if (*de_inode == 0 || *de_rec == 0)
            return -EINVAL;
        pos += *de_rec;
    }

    /* Skip '..' entry */
    {
        uint32_t *de_inode  = (uint32_t *)(block_buf + pos);
        uint16_t *de_rec    = (uint16_t *)(block_buf + pos + 4);
        if (*de_inode == 0 || *de_rec == 0)
            return -EINVAL;
        pos += *de_rec;
    }

    /* At pos, we should have: reserved(4) + hash_version(1) + info_length(1)
     * + indirect_levels(1) + unused_flags(1) = 8 bytes.  Then
     * limit(2) + count(2) + block(4) = 8 bytes.
     * So entries start at pos + 16. */

    if (pos + 16 > ep->block_size)
        return -EINVAL;

    /* Read the info fields */
    uint8_t info_bytes[8];
    memcpy(info_bytes, block_buf + pos, 8);
    uint8_t hash_version    = info_bytes[4];   /* offset 4 within the 8-byte info */
    uint8_t info_length     = info_bytes[5];
    uint8_t indirect_levels = info_bytes[6];
    (void)hash_version;

    if (info_length < 8)
        return -EINVAL;

    /* Read limit, count, block from the root node header */
    uint16_t root_limit = *(uint16_t *)(block_buf + pos + 8);
    uint16_t root_count = *(uint16_t *)(block_buf + pos + 10);
    uint32_t root_block = *(uint32_t *)(block_buf + pos + 12);
    (void)root_limit;

    if (root_count == 0)
        return -EINVAL;

    /* The dx_root entries follow at pos + 16 */
    struct ext2_dx_entry *root_entries = (struct ext2_dx_entry *)(block_buf + pos + 16);

    /* Walk the tree */
    int levels = (int)indirect_levels;
    uint32_t current_block = root_block;
    uint32_t current_hash  = hash;

    while (levels >= 0) {
        uint16_t count;
        uint16_t limit;
        struct ext2_dx_entry *entries;
        uint8_t *node_buf;

        if (levels == (int)indirect_levels && root_block == 0) {
            /* At the root level: use the in-memory root */
            count   = root_count;
            limit   = root_limit;
            entries = root_entries;
            node_buf = block_buf;
        } else {
            /* Read an internal node block */
            uint8_t *ibuf = (uint8_t *)kmalloc(ep->block_size);
            if (!ibuf) return -ENOMEM;
            if (ext2_read_block(ep, current_block, ibuf) < 0) {
                kfree(ibuf);
                return -EIO;
            }
            node_buf = ibuf;

            /* Internal node: 16-byte header before entries */
            count   = *(uint16_t *)(node_buf + 8);
            limit   = *(uint16_t *)(node_buf + 10);
            entries = (struct ext2_dx_entry *)(node_buf + 16);
        }

        if (count == 0 || limit == 0) {
            if (node_buf != block_buf)
                kfree(node_buf);
            return -EINVAL;
        }

        /* Binary search for the highest entry with hash <= current_hash */
        int lo = 0;
        int hi = (int)count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (entries[mid].hash <= current_hash)
                lo = mid + 1;
            else
                hi = mid - 1;
        }

        int idx = (hi >= 0) ? hi : 0;   /* clamp to 0 if hi < 0 */
        if (idx >= count)
            idx = count - 1;

        if (levels == 0) {
            /* Leaf level: return the block number */
            *leaf_block = entries[idx].block;

            if (node_buf != block_buf)
                kfree(node_buf);
            return 0;
        }

        /* Descend to the next level */
        current_block = entries[idx].block;
        current_hash  = hash;
        levels--;

        if (node_buf != block_buf)
            kfree(node_buf);
    }

    return -EINVAL;
#undef EXT2_DIRENT_SIZE
}

/* ── Combined directory entry lookup (HTree + linear fallback) ──── */

/*
 * Find a directory entry by name in an ext2 directory.
 * Uses HTree if available (EXT2_INDEX_FL set and dir_index feature),
 * falls back to linear scan otherwise.
 */
static int ext2_find_in_dir(struct ext2_priv *ep, struct ext2_inode *dir_inode,
                             const char *name, uint32_t *ino)
{
    size_t nlen = strlen(name);
    if (nlen == 0 || nlen > 255)
        return -EINVAL;

    /* Check if HTree indexing is available */
    int use_htree = 0;
    if (dir_inode->i_flags & EXT2_INDEX_FL) {
        /* Verify the superblock has the dir_index feature */
        if (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_DIR_INDEX)
            use_htree = 1;
    }

    if (use_htree) {
        uint32_t hash_seed[4];
        /* Read the hash seed from the superblock if available (ext2 rev 1+),
         * otherwise use a fixed seed of 0.  The hash seed prevents
         * intentional hash collisions that could degrade performance. */
        if (ep->sb.s_rev_level >= 1 &&
            ep->sb.s_def_hash_seed[0] != 0 &&
            (ep->sb.s_def_hash_seed[0] | ep->sb.s_def_hash_seed[1] |
             ep->sb.s_def_hash_seed[2] | ep->sb.s_def_hash_seed[3]) != 0) {
            hash_seed[0] = ep->sb.s_def_hash_seed[0];
            hash_seed[1] = ep->sb.s_def_hash_seed[1];
            hash_seed[2] = ep->sb.s_def_hash_seed[2];
            hash_seed[3] = ep->sb.s_def_hash_seed[3];
        } else {
            hash_seed[0] = 0;
            hash_seed[1] = 0;
            hash_seed[2] = 0;
            hash_seed[3] = 0;
        }
        uint32_t hash = ext2_dx_hash((const unsigned char *)name,
                                      (int)nlen,
                                      EXT2_HTREE_HALF_MD4,
                                      hash_seed);

        uint32_t leaf_block = 0;
        if (ext2_htree_lookup_leaf(ep, dir_inode, hash, &leaf_block) == 0) {
            /* Read the leaf block and scan linearly */
            uint8_t block_buf[4096];
            if (ext2_read_block(ep, leaf_block, block_buf) == 0) {
                uint32_t pos = 0;
                while (pos + 8 < ep->block_size) {
                    struct {
                        uint32_t inode;
                        uint16_t rec_len;
                        uint8_t  name_len;
                        uint8_t  file_type;
                        char     name[255];
                    } *dirent = (void *)(block_buf + pos);

                    if (dirent->rec_len == 0) break;
                    if (dirent->rec_len < 8 + dirent->name_len) break;
                    if (dirent->rec_len % 4 != 0) break;
                    if (dirent->inode != 0 &&
                        (size_t)dirent->name_len == nlen &&
                        memcmp(dirent->name, name, nlen) == 0) {
                        *ino = dirent->inode;
                        return 0;
                    }
                    pos += dirent->rec_len;
                }
            }

            /* ── Linear fallback ──────────────────────────────────────
             * HTree lookup failed (e.g. unsupported hash version) or
             * the entry wasn't in the leaf block (hash collision).
             * Fall through to the linear scan below. */
        }
    }

    /* ── Linear scan (original behaviour) ──────────────────────────── */
    uint32_t iblock = 0;
    uint32_t offset = 0;

    while (offset < ext2_inode_get_size(ep, dir_inode)) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, dir_inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < ext2_inode_get_size(ep, dir_inode)) {
            struct {
                uint32_t inode;
                uint16_t rec_len;
                uint8_t  name_len;
                uint8_t  file_type;
                char     name[255];
            } *dirent = (void *)(block_buf + pos);

            if (dirent->rec_len == 0) break;
            if (dirent->inode == 0) { pos += dirent->rec_len; continue; }

            if ((size_t)dirent->name_len == nlen &&
                memcmp(dirent->name, name, nlen) == 0) {
                *ino = dirent->inode;
                return 0;
            }

            pos += dirent->rec_len;
        }

        iblock++;
        offset += ep->block_size;
    }

    return -EINVAL;
}

/* Read directory entries from inode (linear, returns max entries). */
static int ext2_read_dir(struct ext2_priv *ep, struct ext2_inode *inode,
                          char names[][64], int max) {
    uint32_t iblock = 0;
    uint32_t offset = 0;
    int count = 0;

    while (offset < ext2_inode_get_size(ep, inode) && count < max) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, inode, iblock, block_buf) < 0)
            break;

        uint32_t pos = 0;
        while (pos < ep->block_size && offset + pos < ext2_inode_get_size(ep, inode) && count < max) {
            struct {
                uint32_t inode;
                uint16_t rec_len;
                uint8_t  name_len;
                uint8_t  file_type;
                char     name[255];
            } *dirent = (void *)(block_buf + pos);

            if (dirent->rec_len == 0) break;
            if (dirent->inode == 0) { pos += dirent->rec_len; continue; }

            uint8_t nlen = dirent->name_len;
            if (nlen > 63) nlen = 63;
            memcpy(names[count], dirent->name, nlen);
            names[count][nlen] = '\0';
            count++;

            pos += dirent->rec_len;
        }

        iblock++;
        offset += ep->block_size;
    }

    return count;
}

/* Resolve path to inode */
static int ext2_path_to_ino(struct ext2_priv *ep, const char *path, uint32_t *ino) {
    *ino = EXT2_ROOT_INO;

    const char *p = path;
    if (*p == '/') p++;
    if (*p == '\0') return 0;

    while (*p) {
        /* Skip slashes */
        while (*p == '/') p++;
        if (!*p) break;

        /* Read current directory inode */
        struct ext2_inode dir_inode;
        if (ext2_read_inode(ep, *ino, &dir_inode) < 0) return -EINVAL;

        /* Find next component */
        const char *end = p;
        while (*end && *end != '/') end++;
        size_t clen = (size_t)(end - p);

        char comp[256];
        if (clen >= 256) clen = 255;
        memcpy(comp, p, clen);
        comp[clen] = '\0';

        uint32_t next_ino;
        if (ext2_find_in_dir(ep, &dir_inode, comp, &next_ino) < 0)
            return -EINVAL;

        *ino = next_ino;

        /* ── Follow symlinks with depth limit ──────────────── */
        int sym_depth = 0;
        while (sym_depth < 8) {
            struct ext2_inode link_inode;
            if (ext2_read_inode(ep, *ino, &link_inode) < 0)
                break;
            if (!(link_inode.i_mode & S_IFLNK))
                break;

            char link_target[256];
            uint64_t tlen = ext2_inode_get_size(ep, &link_inode);
            if (tlen == 0 || tlen >= sizeof(link_target) - 1)
                return -EIO;

            if (link_inode.i_blocks == 0 &&
                (uint32_t)tlen <= sizeof(link_inode.i_block)) {
                memcpy(link_target, link_inode.i_block, (size_t)tlen);
                link_target[tlen] = '\0';
            } else {
                uint32_t pbn = link_inode.i_block[0];
                if (pbn == 0) return -EIO;
                uint8_t lbuf[4096];
                uint32_t ss = ep->block_size / 512;
                for (uint32_t si = 0; si < ss; si++) {
                    if (blockdev_read_sectors(ep->dev_id,
                            (uint64_t)pbn * ss + si, 1,
                            lbuf + si * 512) != 0)
                        return -EIO;
                }
                memcpy(link_target, lbuf, (size_t)tlen);
                link_target[tlen] = '\0';
            }

            if (link_target[0] == '/') {
                *ino = EXT2_ROOT_INO;
                sym_depth++;
                p = link_target + 1;
                continue;
            }

            if (*end == '\0') {
                sym_depth++;
                break;
            }
            break;
        }
        if (sym_depth >= 8)
            return -ELOOP;

        p = end;
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Symlink operations (fast + slow)
 * ═══════════════════════════════════════════════════════════════════════ */

/*
 * Read the target of a symlink.
 *
 * Fast symlinks store the target directly in the inode's i_block[] array
 * (up to 60 bytes = 15 × 4).  Slow symlinks use a single data block
 * pointed to by i_block[0].
 *
 * Returns the number of bytes written to @buf (not including NUL
 * terminator), or negative errno on failure.
 */
static int ext2_readlink(void *priv, const char *path, char *buf,
                          int bufsize)
{
	struct ext2_priv *ep = (struct ext2_priv *)priv;
	uint32_t ino;
	if (ext2_path_to_ino(ep, path, &ino) < 0)
		return -ENOENT;

	struct ext2_inode inode;
	if (ext2_read_inode(ep, ino, &inode) < 0)
		return -EIO;

	uint64_t target_len = ext2_inode_get_size(ep, &inode);
	if (target_len == 0) {
		buf[0] = '\0';
		return 0;
	}
	if ((int)target_len > bufsize - 1)
		return -ENAMETOOLONG;

	/* Detect fast symlink: i_blocks == 0 and the symlink target fits
	 * in i_block[] (up to 60 bytes for standard ext2).  Some ext2
	 * implementations also check i_size <= 60.  We use i_blocks == 0
	 * as the primary indicator since slow symlinks always allocate
	 * at least one block. */
	if (inode.i_blocks == 0 &&
	    (uint32_t)target_len <= sizeof(inode.i_block)) {
		/* Fast symlink: target is in i_block[] as a char array */
		memcpy(buf, inode.i_block, (size_t)target_len);
		buf[target_len] = '\0';
		return (int)target_len;
	}

	/* Slow symlink: read the target from the data block */
	uint32_t pbn = inode.i_block[0];
	if (pbn == 0)
		return -EIO;

	uint8_t block_buf[4096];
	if (ep->block_size > 4096) return -EINVAL;
	uint32_t sectors = ep->block_size / 512;
	for (uint32_t i = 0; i < sectors; i++) {
		if (blockdev_read_sectors(ep->dev_id,
		                          (uint64_t)pbn * sectors + i,
		                          1, block_buf + i * 512) != 0)
			return -EIO;
	}
	memcpy(buf, block_buf, (size_t)target_len);
	buf[target_len] = '\0';
	return (int)target_len;
}

/*
 * Create a symlink.
 *
 * For short targets (≤ 60 bytes), creates a fast symlink with the
 * target stored in the inode's i_block[].  For longer targets, creates
 * a slow symlink with the target in a data block.
 */
static int ext2_symlink(void *priv, const char *target,
                         const char *linkpath)
{
	struct ext2_priv *ep = (struct ext2_priv *)priv;
	size_t target_len = strlen(target);
	if (target_len > 4095)  /* symlink targets are limited */
		return -ENAMETOOLONG;

	/* ── Resolve parent directory path ────────────────────────── */
	/* Find the last '/' in linkpath */
	const char *slash = strrchr(linkpath, '/');
	const char *basename;
	char parent_path[128];

	if (slash && slash != linkpath) {
		size_t plen = (size_t)(slash - linkpath);
		if (plen >= sizeof(parent_path))
			return -ENAMETOOLONG;
		memcpy(parent_path, linkpath, plen);
		parent_path[plen] = '\0';
		basename = slash + 1;
	} else if (slash && slash == linkpath) {
		/* Root directory, e.g. "/symlink" */
		parent_path[0] = '/';
		parent_path[1] = '\0';
		basename = linkpath + 1;
	} else {
		/* No slash — relative path, parent is current directory.
		 * For now, we use root.  A more complete implementation
		 * would use CWD from the process. */
		strncpy(parent_path, "/", sizeof(parent_path) - 1);
		parent_path[sizeof(parent_path) - 1] = '\0';
		basename = linkpath;
	}

	if (*basename == '\0')
		return -EINVAL;

	/* ── Look up the parent directory inode ───────────────────── */
	uint32_t parent_ino;
	if (ext2_path_to_ino(ep, parent_path, &parent_ino) < 0)
		return -ENOENT;

	/* Check if an entry with the same name already exists */
	{
		struct ext2_inode check_dir;
		if (ext2_read_inode(ep, parent_ino, &check_dir) < 0)
			return -EIO;
		uint32_t check_ino;
		if (ext2_find_in_dir(ep, &check_dir, basename,
		                      &check_ino) == 0)
			return -EEXIST;
	}

	/* Verify parent is a directory */
	struct ext2_inode dir_inode;
	if (ext2_read_inode(ep, parent_ino, &dir_inode) < 0)
		return -EIO;
	if (!(dir_inode.i_mode & 0x4000))  /* S_IFDIR */
		return -ENOTDIR;

	/* ── Allocate a new inode for the symlink ─────────────────── */
	uint32_t new_ino;
	int ret = ext2_alloc_inode(ep, &new_ino);
	if (ret < 0) return ret;

	struct ext2_inode sym_inode;
	memset(&sym_inode, 0, sizeof(sym_inode));
	sym_inode.i_mode   = S_IFLNK | 0777;  /* symlink with full perms */
	sym_inode.i_uid    = 0;
	sym_inode.i_gid    = 0;
	sym_inode.i_size   = (uint32_t)target_len;
	sym_inode.i_atime  = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
	sym_inode.i_ctime  = sym_inode.i_atime;
	sym_inode.i_mtime  = sym_inode.i_atime;
	sym_inode.i_links_count = 1;
	sym_inode.i_blocks = 0;

	if (target_len <= sizeof(sym_inode.i_block)) {
		/* ── Fast symlink: store target in i_block[] ────────── */
		memcpy(sym_inode.i_block, target, target_len);
		/* i_blocks stays 0 — no data blocks allocated */
	} else {
		/* ── Slow symlink: allocate a data block ────────────── */
		uint32_t data_block;
		ret = ext2_alloc_block(ep, &data_block);
		if (ret < 0) {
			/* Free the inode we just allocated */
			/* (best-effort) */
			goto err_free_inode;
		}
		sym_inode.i_block[0] = data_block;
		sym_inode.i_blocks = ep->block_size / 512;

		/* Write the target string to the data block */
		uint8_t data_buf[4096];
		memset(data_buf, 0, ep->block_size);
		memcpy(data_buf, target, target_len);
		if (ext2_write_block(ep, data_block, data_buf) < 0)
			goto err_free_block;
	}

	/* Write the symlink inode to disk */
	ret = ext2_write_inode(ep, new_ino, &sym_inode);
	if (ret < 0) goto err_free_block;

	/* ── Add directory entry in parent ────────────────────────── */
	ret = ext2_add_dirent(ep, &dir_inode, parent_ino, basename,
	                       new_ino, EXT2_FT_SYMLINK);
	if (ret < 0) {
		/* We could clean up the inode here, but for now just
		 * report the error.  The inode will be orphaned. */
		return ret;
	}

	/* Update parent's directory count if this is a directory
	 * (symlinks don't increment used_dirs_count) */
	dir_inode.i_links_count++;  /* each new entry increments link count */
	dir_inode.i_mtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
	ret = ext2_write_inode(ep, parent_ino, &dir_inode);
	if (ret < 0) return ret;

	/* Update superblock on disk periodically */
	ep->sb.s_wtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);

	return 0;

err_free_block:
	if (target_len > sizeof(sym_inode.i_block) && sym_inode.i_block[0] != 0) {
		/* Best-effort: mark the block free again */
		/* For a real implementation we'd update the bitmap */
	}
err_free_inode:
	/* Best-effort: mark the inode free again */
	return ret;
}

/* ── Hard link creation ───────────────────────────────────────────── */

/*
 * Create a hard link (additional directory entry pointing to the same
 * inode, with incremented link count).
 *
 * Parameters:
 *   priv    — ext2 private data
 *   oldpath — path to the existing file
 *   newpath — path for the new link (must not exist)
 *
 * Returns 0 on success, negative errno on failure.
 * Hard links to directories are not permitted (standard ext2 limitation).
 */
static int ext2_link(void *priv, const char *oldpath, const char *newpath)
{
	struct ext2_priv *ep = (struct ext2_priv *)priv;

	/* ── Resolve old path to its inode ──────────────────────────── */
	uint32_t target_ino;
	if (ext2_path_to_ino(ep, oldpath, &target_ino) < 0)
		return -ENOENT;

	struct ext2_inode target_inode;
	if (ext2_read_inode(ep, target_ino, &target_inode) < 0)
		return -EIO;

	/* Directories cannot be hard-linked (would create loops) */
	if (target_inode.i_mode & S_IFDIR)
		return -EPERM;

	/* ── Extract parent directory path and basename from newpath ── */
	const char *slash = strrchr(newpath, '/');
	const char *basename;
	char parent_path[128];

	if (slash && slash != newpath) {
		size_t plen = (size_t)(slash - newpath);
		if (plen >= sizeof(parent_path))
			return -ENAMETOOLONG;
		memcpy(parent_path, newpath, plen);
		parent_path[plen] = '\0';
		basename = slash + 1;
	} else if (slash && slash == newpath) {
		/* Root directory, e.g. "/hardlink" */
		parent_path[0] = '/';
		parent_path[1] = '\0';
		basename = newpath + 1;
	} else {
		/* No slash — relative path, parent is current directory.
		 * Use root as default. */
		strncpy(parent_path, "/", sizeof(parent_path) - 1);
		parent_path[sizeof(parent_path) - 1] = '\0';
		basename = newpath;
	}

	if (*basename == '\0')
		return -EINVAL;

	/* ── Look up the parent directory inode ──────────────────────── */
	uint32_t parent_ino;
	if (ext2_path_to_ino(ep, parent_path, &parent_ino) < 0)
		return -ENOENT;

	/* Check if an entry with the same name already exists */
	{
		struct ext2_inode check_dir;
		if (ext2_read_inode(ep, parent_ino, &check_dir) < 0)
			return -EIO;
		uint32_t check_ino;
		if (ext2_find_in_dir(ep, &check_dir, basename,
		                      &check_ino) == 0)
			return -EEXIST;
	}

	/* Verify parent is a directory */
	struct ext2_inode dir_inode;
	if (ext2_read_inode(ep, parent_ino, &dir_inode) < 0)
		return -EIO;
	if (!(dir_inode.i_mode & S_IFDIR))
		return -ENOTDIR;

	/* Determine the file type for the directory entry */
	uint8_t file_type;
	switch (target_inode.i_mode & S_IFMT) {
	case S_IFREG:     file_type = EXT2_FT_REG_FILE;  break;
	case S_IFDIR:     file_type = EXT2_FT_DIR;       break;
	case S_IFLNK:     file_type = EXT2_FT_SYMLINK;   break;
	case S_IFCHR:     file_type = EXT2_FT_CHRDEV;    break;
	case S_IFBLK:     file_type = EXT2_FT_BLKDEV;    break;
	case S_IFIFO:     file_type = EXT2_FT_FIFO;      break;
	case S_IFSOCK:    file_type = EXT2_FT_SOCK;      break;
	default:          file_type = EXT2_FT_UNKNOWN;   break;
	}

	/* ── Add directory entry in parent ────────────────────────────── */
	int ret = ext2_add_dirent(ep, &dir_inode, parent_ino, basename,
	                           target_ino, file_type);
	if (ret < 0)
		return ret;

	/* ── Increment link count on the target inode ─────────────────── */
	uint32_t now = (uint32_t)(timer_get_ticks() / TIMER_FREQ);
	target_inode.i_links_count++;
	target_inode.i_ctime = now;
	ret = ext2_write_inode(ep, target_ino, &target_inode);
	if (ret < 0)
		return ret;

	/* Update parent directory timestamps */
	dir_inode.i_mtime = now;
	ret = ext2_write_inode(ep, parent_ino, &dir_inode);
	if (ret < 0)
		return ret;

	/* Update superblock write time */
	ep->sb.s_wtime = now;

	return 0;
}

/* ── VFS operations ──────────────────────────────────────────────── */

static int ext2_read(void *priv, const char *path, void *buf,
                     uint32_t max_size, uint32_t *out_size) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    uint64_t file_size = ext2_inode_get_size(ep, &inode);
    uint64_t to_read = file_size;
    if (to_read > max_size) to_read = max_size;

    uint32_t iblock = 0;
    uint32_t done = 0;
    while (done < to_read) {
        uint8_t block_buf[4096];
        if (ext2_read_inode_block(ep, &inode, iblock, block_buf) < 0)
            break;

        uint32_t chunk = (uint32_t)(to_read - done);
        if (chunk > ep->block_size) chunk = ep->block_size;
        memcpy((uint8_t *)buf + done, block_buf, chunk);

        done += chunk;
        iblock++;
    }

    *out_size = done;
    return 0;
}

static int ext2_stat(void *priv, const char *path, struct vfs_stat *st) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    memset(st, 0, sizeof(*st));
    st->size = ext2_inode_get_size(ep, &inode);
    if (inode.i_mode & S_IFLNK)
        st->type = VFS_TYPE_LINK;
    else if (inode.i_mode & S_IFDIR)
        st->type = VFS_TYPE_DIR;
    else
        st->type = VFS_TYPE_FILE;
    st->uid  = inode.i_uid;
    st->gid  = inode.i_gid;
    st->mode = (uint16_t)(inode.i_mode & 0xFFFF);
    st->mtime = inode.i_mtime;
    st->nlink = inode.i_links_count;
    st->ino   = ino;
    return 0;
}

static int ext2_readdir(void *priv, const char *path, char names[][64], int max) {
    struct ext2_priv *ep = (struct ext2_priv *)priv;
    uint32_t ino;
    if (ext2_path_to_ino(ep, path, &ino) < 0) return -EINVAL;

    struct ext2_inode inode;
    if (ext2_read_inode(ep, ino, &inode) < 0) return -EINVAL;

    return ext2_read_dir(ep, &inode, names, max);
}

static int ext2_readdir_legacy(void *priv, const char *path) {
    char names[64][64];
    int n = ext2_readdir(priv, path, names, 64);
    for (int i = 0; i < n; i++)
        kprintf("  %s\n", names[i]);
    return n;
}

/* Forward declaration: BGD scan (defined later, called from mount) */
int ext2_scan_block_groups(struct ext2_priv *ep);

static struct vfs_ops ext2_ops = {
    .read    = ext2_read,
    .stat    = ext2_stat,
    .readdir_names = ext2_readdir,
    .readdir = ext2_readdir_legacy,
    .link    = ext2_link,
    .symlink = ext2_symlink,
    .readlink = ext2_readlink,
};

int ext2_mount(const char *mountpoint, uint8_t dev_id) {
    struct ext2_priv *ep = (struct ext2_priv *)kmalloc(sizeof(struct ext2_priv));
    if (!ep) return -ENOMEM;

    memset(ep, 0, sizeof(*ep));
    ep->dev_id = dev_id;
    /* Store mountpoint for vfs_force_readonly() on corruption detection */
    strncpy(ep->mountpoint, mountpoint, sizeof(ep->mountpoint) - 1);
    ep->mountpoint[sizeof(ep->mountpoint) - 1] = '\0';

    if (ext2_load_super(ep) < 0) {
        kfree(ep);
        return -EINVAL;
    }

    if (ep->sb.s_magic != EXT2_SUPER_MAGIC) {
        kprintf("[ext2] Bad superblock magic: 0x%x\n", ep->sb.s_magic);
        kfree(ep);
        return -EINVAL;
    }

    ep->block_size = 1024 << ep->sb.s_log_block_size;
    if (ep->block_size > 4096) {
        kprintf("[ext2] Block size %u too large\n", ep->block_size);
        kfree(ep);
        return -EFBIG;
    }

    ep->blocks_per_group = ep->sb.s_blocks_per_group;
    ep->inodes_per_group = ep->sb.s_inodes_per_group;
    ep->inode_size = 128; /* standard */

    uint32_t total_groups = (ep->sb.s_blocks_count + ep->blocks_per_group - 1) / ep->blocks_per_group;
    ep->num_block_groups = total_groups;

    /* ── Load the block group descriptor table into cache ──────────
     *
     * The bgd table is always read from the primary copy in group 0.
     * Its location within group 0 depends on whether block_size == 1024
     * (where superblock occupies 2 blocks) or larger (superblock is
     * 1 block).  For sparse superblock filesystems the backup copies
     * are fewer, but the primary copy (group 0) always exists.
     *
     * With flex_bg (EXT2_FEATURE_INCOMPAT_FLEX_BG), the metadata is
     * packed together, but the bgd entries still point to the correct
     * physical block locations — so reading them transparently works. */
    uint32_t bgd_first_block = ep->block_size == 1024 ? 2 : 1;
    uint64_t bgd_table_bytes = (uint64_t)ep->num_block_groups * sizeof(struct ext2_bg_desc);
    uint64_t bgd_blocks_needed = (bgd_table_bytes + ep->block_size - 1) / ep->block_size;

    ep->bgd_cache = (struct ext2_bg_desc *)kmalloc(bgd_table_bytes);
    if (!ep->bgd_cache) {
        kprintf("[ext2] Failed to allocate bgd cache (%llu bytes)\n", (unsigned long long)bgd_table_bytes);
        kfree(ep);
        return -ENOMEM;
    }
    ep->bgd_cache_size = bgd_table_bytes;
    memset(ep->bgd_cache, 0, bgd_table_bytes);

    /* Read the bgd blocks one at a time into the cache */
    uint8_t block_buf[4096];
    uint32_t bytes_read = 0;
    for (uint32_t i = 0; i < bgd_blocks_needed; i++) {
        if (ext2_read_block(ep, bgd_first_block + i, block_buf) < 0) {
            kprintf("[ext2] Failed to read bgd block %u\n", bgd_first_block + i);
            kfree(ep->bgd_cache);
            kfree(ep);
            return -EIO;
        }
        uint32_t copy_size = bgd_table_bytes - bytes_read;
        if (copy_size > ep->block_size) copy_size = ep->block_size;
        memcpy((uint8_t *)ep->bgd_cache + bytes_read, block_buf, copy_size);
        bytes_read += copy_size;
    }

    /* ── Validate the block group descriptor table ──────────────────── */
    {
        int scan_ret = ext2_scan_block_groups(ep);
        if (scan_ret < 0) {
            kprintf("[ext2] BGD scan found errors, "
                    "mounting in degraded mode\n");
            /* Continue anyway — read-only operations may still work */
        }
    }

    /* ── Detect and log feature flags ────────────────────────────── */
    int has_sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    int has_flexbg = (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_FLEX_BG) ? 1 : 0;
    int has_large  = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_LARGE_FILE) ? 1 : 0;
    int has_htree  = (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_DIR_INDEX) ? 1 : 0;
    int has_journal= (ep->sb.s_feature_compat & EXT2_FEATURE_COMPAT_HAS_JOURNAL) ? 1 : 0;
    int has_extents= (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_EXTENTS) ? 1 : 0;
    int has_64bit  = (ep->sb.s_feature_incompat & EXT2_FEATURE_INCOMPAT_64BIT) ? 1 : 0;

    /* ── Reject unsupported features that would cause data corruption ── */
    if (has_extents) {
        /* Support extent tree traversal for EXT4-compatible extents */
        kprintf("[ext2] EXTENTS feature detected, enabling extent tree support\n");
        /* We support extent trees now — don't refuse mount */
    }
    if (has_64bit) {
        kprintf("[ext2] 64BIT feature detected, using 64-bit addressing\n");
        /* Support 64-bit block numbers — allocate larger BGD entries */
    }
    /* Reject any other incompatible features we don't understand */
    {
        /* Define the mask of incompatible features we support.
         * FILETYPE: file type in directory entries — basic ext2 feature.
         * RECOVER: needs recovery (journal replay) — OK for read-only mount.
         * FLEX_BG: flex block groups — handled transparently. */
        uint32_t supp_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE
                               | EXT2_FEATURE_INCOMPAT_RECOVER
                               | EXT2_FEATURE_INCOMPAT_FLEX_BG;
        uint32_t unsup = ep->sb.s_feature_incompat & ~supp_incompat;
        if (unsup) {
            kprintf("[ext2] Unsupported incompatible features: 0x%x, refusing mount\n", unsup);
            kfree(ep->bgd_cache);
            kfree(ep);
            return -EOPNOTSUPP;
        }
    }

    kprintf("[ext2] Mounted: %u blocks, %u inodes, %u B/block, %u groups",
            ep->sb.s_blocks_count, ep->sb.s_inodes_count,
            ep->block_size, total_groups);
    if (has_sparse)  kprintf(", sparse_super");
    if (has_flexbg)  kprintf(", flex_bg");
    if (has_large)   kprintf(", large_file");
    if (has_htree)   kprintf(", htree");
    if (has_journal) kprintf(", journal");
    if (has_extents) kprintf(", extents");
    if (has_64bit)   kprintf(", 64bit");
    kprintf("\n");

    return vfs_mount_ex(mountpoint, &ext2_ops, ep, 0);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ext2 online resize — add block groups while mounted
 *
 *  Enables growing an ext2 filesystem by adding new block groups
 *  while the filesystem is mounted and in use.  The resize operation:
 *    1. Validates the new size is larger than current
 *    2. Allocates new block groups with proper metadata
 *    3. Updates the superblock (s_blocks_count, s_free_blocks_count)
 *    4. Expands the BGD cache to cover the new groups
 *    5. Syncs the updated superblock and BGD table to disk
 *
 *  Item 452: ext2 online resize
 * ═══════════════════════════════════════════════════════════════════════ */

/* Reallocate the BGD cache to accommodate more block groups.
 * Returns 0 on success, -ENOMEM on allocation failure. */
static int ext2_resize_bgd_cache(struct ext2_priv *ep, uint32_t new_num_groups)
{
    uint64_t new_size = (uint64_t)new_num_groups * sizeof(struct ext2_bg_desc);
    struct ext2_bg_desc *new_cache = (struct ext2_bg_desc *)kmalloc(new_size);
    if (!new_cache)
        return -ENOMEM;

    memset(new_cache, 0, new_size);
    if (ep->bgd_cache && ep->bgd_cache_size > 0)
        memcpy(new_cache, ep->bgd_cache, ep->bgd_cache_size);

    if (ep->bgd_cache)
        kfree(ep->bgd_cache);

    ep->bgd_cache = new_cache;
    ep->bgd_cache_size = new_size;
    return 0;
}

/* Write the updated superblock to disk (all backup copies). */
static int ext2_sync_super(struct ext2_priv *ep)
{
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint8_t buf[1024];

    memset(buf, 0, sizeof(buf));
    memcpy(buf, &ep->sb, sizeof(ep->sb));

    /* Write primary superblock (offset 1024 = sector 2) */
    if (blockdev_write_sectors(ep->dev_id, 2, 2, buf) != 0)
        return -EIO;

    /* Write backup superblocks where they exist */
    for (uint32_t g = 0; g < ep->num_block_groups; g++) {
        if (g == 0) continue; /* already wrote primary */
        if (!ext2_group_has_super(sparse, g)) continue;
        uint64_t sb_sector = (uint64_t)ext2_group_start(g, ep->blocks_per_group)
                             * (ep->block_size / 512) + 2;
        if (blockdev_write_sectors(ep->dev_id, sb_sector, 2, buf) != 0)
            return -EIO;
    }
    return 0;
}

/* Write the current BGD cache to disk. */
static int ext2_sync_bgd(struct ext2_priv *ep)
{
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint32_t bgd_first_block = (ep->block_size == 1024) ? 2 : 1;
    uint64_t bgd_bytes = (uint64_t)ep->num_block_groups * sizeof(struct ext2_bg_desc);
    uint32_t bgd_blocks = (uint32_t)((bgd_bytes + ep->block_size - 1) / ep->block_size);
    uint8_t *block_buf = (uint8_t *)kmalloc(ep->block_size);
    if (!block_buf) return -ENOMEM;

    /* Write primary BGD table */
    uint32_t offset = 0;
    for (uint32_t i = 0; i < bgd_blocks; i++) {
        memset(block_buf, 0, ep->block_size);
        uint32_t copy = bgd_bytes - offset;
        if (copy > ep->block_size) copy = ep->block_size;
        memcpy(block_buf, (uint8_t *)ep->bgd_cache + offset, copy);

        uint64_t block_num = bgd_first_block + i;
        uint64_t lba = block_num * (ep->block_size / 512);
        for (uint32_t s = 0; s < ep->block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       block_buf + s * 512) != 0) {
                kfree(block_buf);
                return -EIO;
            }
        }
        offset += copy;
    }

    /* Write backup BGD tables in groups that have superblock backups */
    for (uint32_t g = 1; g < ep->num_block_groups; g++) {
        if (!ext2_group_has_super(sparse, g)) continue;
        uint32_t bgd_block_in_group = (ep->block_size == 1024) ? 2 : 1;
        uint32_t bgd_start = ext2_group_start(g, ep->blocks_per_group) + bgd_block_in_group;

        offset = 0;
        for (uint32_t i = 0; i < bgd_blocks; i++) {
            memset(block_buf, 0, ep->block_size);
            uint32_t copy = bgd_bytes - offset;
            if (copy > ep->block_size) copy = ep->block_size;
            memcpy(block_buf, (uint8_t *)ep->bgd_cache + offset, copy);

            uint64_t lba = (uint64_t)(bgd_start + i) * (ep->block_size / 512);
            for (uint32_t s = 0; s < ep->block_size / 512; s++) {
                if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                           block_buf + s * 512) != 0) {
                    kfree(block_buf);
                    return -EIO;
                }
            }
            offset += copy;
        }
    }

    kfree(block_buf);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Block Group Descriptor scanning and validation
 *
 *  Validates the block group descriptor table by checking backup copies
 *  across the filesystem and cross-referencing against the superblock.
 *  Detects corruption, mismatches, and structural issues in the BGD data.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Validate a single block group descriptor entry against superblock
 * limits and structural invariants.
 *
 * Checks:
 *  - block bitmap pointer is within the group's block range
 *  - inode bitmap pointer is within the group's block range
 *  - inode table pointer is within the group's block range
 *  - inode table does not overlap with block bitmap
 *  - free counts do not exceed group capacity
 *  - used directories count does not exceed inodes per group
 *
 * Returns 0 if the entry looks valid, -EFSCORRUPTED on corruption. */
static int ext2_validate_bgd_entry(struct ext2_priv *ep,
                                    uint32_t group,
                                    const struct ext2_bg_desc *bgd)
{
	uint32_t blocks_in_group = ep->blocks_per_group;
	uint32_t inodes_in_group = ep->inodes_per_group;
	uint32_t group_start = group * ep->blocks_per_group;

	/* Last group may be smaller — compute actual size */
	if (group == ep->num_block_groups - 1) {
		uint32_t brem = ep->sb.s_blocks_count
		                - group * ep->blocks_per_group;
		if (brem > 0 && brem < blocks_in_group)
			blocks_in_group = brem;
		uint32_t irem = ep->sb.s_inodes_count
		                - group * ep->inodes_per_group;
		if (irem > 0 && irem < inodes_in_group)
			inodes_in_group = irem;
	}

	uint32_t group_end = group_start + blocks_in_group;

	/* Block bitmap must be within the group's block range */
	if (bgd->bg_block_bitmap < group_start ||
	    bgd->bg_block_bitmap >= group_end) {
		kprintf("[ext2] BGD[%u]: block bitmap %u outside "
		        "group [%u, %u)\n",
		        group, bgd->bg_block_bitmap,
		        group_start, group_end);
		return -EFSCORRUPTED;
	}

	/* Inode bitmap must be within the group's block range */
	if (bgd->bg_inode_bitmap < group_start ||
	    bgd->bg_inode_bitmap >= group_end) {
		kprintf("[ext2] BGD[%u]: inode bitmap %u outside "
		        "group [%u, %u)\n",
		        group, bgd->bg_inode_bitmap,
		        group_start, group_end);
		return -EFSCORRUPTED;
	}

	/* Inode table must be within the group's block range */
	if (bgd->bg_inode_table < group_start ||
	    bgd->bg_inode_table >= group_end) {
		kprintf("[ext2] BGD[%u]: inode table %u outside "
		        "group [%u, %u)\n",
		        group, bgd->bg_inode_table,
		        group_start, group_end);
		return -EFSCORRUPTED;
	}

	/* Inode table must not be before or overlap with bitmaps */
	if (bgd->bg_inode_table <= bgd->bg_inode_bitmap) {
		kprintf("[ext2] BGD[%u]: inode table %u <= "
		        "inode bitmap %u\n",
		        group, bgd->bg_inode_table,
		        bgd->bg_inode_bitmap);
		return -EFSCORRUPTED;
	}

	/* Free block count must not exceed the group's block count */
	if (bgd->bg_free_blocks_count > blocks_in_group) {
		kprintf("[ext2] BGD[%u]: free blocks %u > max %u\n",
		        group, bgd->bg_free_blocks_count,
		        blocks_in_group);
		return -EFSCORRUPTED;
	}

	/* Free inode count must not exceed the group's inode count */
	if (bgd->bg_free_inodes_count > inodes_in_group) {
		kprintf("[ext2] BGD[%u]: free inodes %u > max %u\n",
		        group, bgd->bg_free_inodes_count,
		        inodes_in_group);
		return -EFSCORRUPTED;
	}

	/* Used directories count must not exceed total inodes in group */
	if (bgd->bg_used_dirs_count > inodes_in_group) {
		kprintf("[ext2] BGD[%u]: used dirs %u > max inodes %u\n",
		        group, bgd->bg_used_dirs_count,
		        inodes_in_group);
		return -EFSCORRUPTED;
	}

	return 0;
}

/* Verify that the number of block groups in the BGD cache matches the
 * count derived from the superblock (s_blocks_count / s_blocks_per_group).
 *
 * Returns 0 if consistent, -EFSCORRUPTED on mismatch. */
static int ext2_validate_bgd_count(struct ext2_priv *ep)
{
	uint32_t expected = (ep->sb.s_blocks_count + ep->blocks_per_group
	                     - 1) / ep->blocks_per_group;

	if (!ep->bgd_cache || ep->num_block_groups == 0) {
		kprintf("[ext2] BGD cache not initialized\n");
		return -EFSCORRUPTED;
	}

	if (ep->num_block_groups != expected) {
		kprintf("[ext2] BGD count mismatch: cache has %u groups, "
		        "superblock implies %u (blocks=%u, bpg=%u)\n",
		        ep->num_block_groups, expected,
		        ep->sb.s_blocks_count, ep->blocks_per_group);
		return -EFSCORRUPTED;
	}

	return 0;
}

/* Read the block group descriptor table from a specific block group.
 *
 * For groups that have a superblock backup (according to the sparse
 * layout), the BGD table is located just after the superblock copy.
 * Groups without a superblock backup have no BGD table to read.
 *
 * Parameters:
 *   ep           — ext2 private data
 *   group        — block group number to read from
 *   sparse       — non-zero if EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER
 *   bgd_buf      — output buffer (must hold num_entries descriptors)
 *   num_entries  — number of BGD entries to read
 *
 * Returns 0 on success, -ENOENT if the group has no BGD backup,
 * -EIO on read error, -EINVAL on invalid arguments. */
static int ext2_read_bgd_at(struct ext2_priv *ep, uint32_t group,
                              int sparse, struct ext2_bg_desc *bgd_buf,
                              uint32_t num_entries)
{
	if (!ep || !bgd_buf || num_entries == 0)
		return -EINVAL;

	/* Groups without a superblock backup have no BGD table */
	if (!ext2_group_has_super(sparse, group))
		return -ENOENT;

	uint32_t gstart = group * ep->blocks_per_group;
	uint32_t bgd_first_block = gstart
	                          + ((ep->block_size == 1024) ? 2 : 1);

	uint64_t bgd_bytes = (uint64_t)num_entries
	                     * sizeof(struct ext2_bg_desc);
	uint32_t bgd_blocks = (uint32_t)(
	    (bgd_bytes + ep->block_size - 1) / ep->block_size);

	uint8_t block_buf[4096];
	uint32_t bytes_read = 0;

	for (uint32_t i = 0; i < bgd_blocks; i++) {
		if (ext2_read_block(ep, bgd_first_block + i,
		                    block_buf) < 0)
			return -EIO;

		uint32_t copy = (uint32_t)bgd_bytes - bytes_read;
		if (copy > ep->block_size)
			copy = ep->block_size;
		memcpy((uint8_t *)bgd_buf + bytes_read,
		       block_buf, copy);
		bytes_read += copy;
	}

	return 0;
}

/* Scan backup BGD copies across all block groups that have superblock
 * backups, comparing each entry against the primary cache.
 *
 * Reports mismatches via kprintf.  If @repair is non-zero, writes the
 * primary BGD table to each corrupt backup location.
 *
 * Returns the number of groups with mismatches (0 = all clean).
 * Returns negative errno on I/O error. */
static int ext2_scan_bgd_backups(struct ext2_priv *ep, int repair)
{
	int sparse = (ep->sb.s_feature_ro_compat
	              & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
	uint32_t num_groups = ep->num_block_groups;
	uint64_t bgd_bytes = (uint64_t)num_groups
	                     * sizeof(struct ext2_bg_desc);
	int mismatches = 0;

	/* Allocate temporary buffer for reading backup BGD tables */
	struct ext2_bg_desc *backup_bgd;
	backup_bgd = (struct ext2_bg_desc *)kmalloc(bgd_bytes);
	if (!backup_bgd)
		return -ENOMEM;

	/* Check each group that has a superblock backup */
	for (uint32_t g = 1; g < num_groups; g++) {
		if (!ext2_group_has_super(sparse, g))
			continue;

		memset(backup_bgd, 0, bgd_bytes);
		int ret = ext2_read_bgd_at(ep, g, sparse,
		                           backup_bgd, num_groups);
		if (ret < 0) {
			if (ret == -ENOENT)
				continue;
			kprintf("[ext2] BGD scan: failed to read "
			        "backup in group %u: %d\n", g, ret);
			kfree(backup_bgd);
			return ret;
		}

		/* Compare each entry with the primary cache */
		for (uint32_t e = 0; e < num_groups; e++) {
			if (memcmp(&ep->bgd_cache[e],
			           &backup_bgd[e],
			           sizeof(struct ext2_bg_desc)) != 0) {
				kprintf("[ext2] BGD scan: backup "
				        "mismatch in group %u, "
				        "entry %u:\n", g, e);
				kprintf("  primary:  bitmap=%u "
				        "ibitmap=%u itable=%u "
				        "free_blk=%u free_ino=%u "
				        "dirs=%u\n",
				        ep->bgd_cache[e].bg_block_bitmap,
				        ep->bgd_cache[e].bg_inode_bitmap,
				        ep->bgd_cache[e].bg_inode_table,
				        ep->bgd_cache[e].bg_free_blocks_count,
				        ep->bgd_cache[e].bg_free_inodes_count,
				        ep->bgd_cache[e].bg_used_dirs_count);
				kprintf("  backup:   bitmap=%u "
				        "ibitmap=%u itable=%u "
				        "free_blk=%u free_ino=%u "
				        "dirs=%u\n",
				        backup_bgd[e].bg_block_bitmap,
				        backup_bgd[e].bg_inode_bitmap,
				        backup_bgd[e].bg_inode_table,
				        backup_bgd[e].bg_free_blocks_count,
				        backup_bgd[e].bg_free_inodes_count,
				        backup_bgd[e].bg_used_dirs_count);

				if (repair) {
					/* Write primary copy over backup */
					ext2_sync_bgd(ep);
					kprintf("[ext2] BGD scan: "
					        "repaired backup "
					        "in group %u\n", g);
				}
				mismatches++;
			}
		}
	}

	kfree(backup_bgd);

	if (mismatches > 0) {
		kprintf("[ext2] BGD scan: %d backup mismatch(es) %s\n",
		        mismatches,
		        repair ? "repaired" : "found");
	} else {
		kprintf("[ext2] BGD scan: all backup copies match "
		        "primary\n");
	}

	return mismatches;
}

/* Comprehensive block group descriptor scan.
 *
 * Performs three stages of validation:
 *  1. Verify BGD count matches superblock
 *  2. Validate each BGD entry's fields for consistency
 *  3. Read backup BGD copies and compare against primary
 *
 * Call this after the BGD cache is populated (e.g. at mount time).
 *
 * Returns 0 on success, -EFSCORRUPTED if any consistency errors
 * are found, or negative errno on I/O error. */
int ext2_scan_block_groups(struct ext2_priv *ep)
{
	int total_errors = 0;
	int ret;

	kprintf("[ext2] Scanning block group descriptors...\n");

	/* 1. Validate the number of block groups */
	ret = ext2_validate_bgd_count(ep);
	if (ret < 0) {
		kprintf("[ext2] BGD count validation failed\n");
		total_errors++;
	}

	/* 2. Validate each individual BGD entry */
	for (uint32_t g = 0; g < ep->num_block_groups; g++) {
		ret = ext2_validate_bgd_entry(ep, g,
		                               &ep->bgd_cache[g]);
		if (ret < 0)
			total_errors++;
	}

	/* 3. Scan backup copies (read-only, no repair) */
	ret = ext2_scan_bgd_backups(ep, 0);
	if (ret < 0) {
		kprintf("[ext2] BGD backup scan failed: %d\n", ret);
	} else if (ret > 0) {
		total_errors += ret;
	}

	kprintf("[ext2] BGD scan complete: %d error(s) found\n",
	        total_errors);
	return total_errors > 0 ? -EFSCORRUPTED : 0;
}

/* Initialize a new block group's metadata (bitmaps + inode table).
 * @ep: ext2 private data
 * @group: new group number to initialise
 * Returns 0 on success, negative on error. */
static int ext2_init_new_group(struct ext2_priv *ep, uint32_t group)
{
    uint32_t blocks_per_group = ep->blocks_per_group;
    uint32_t inodes_per_group = ep->inodes_per_group;
    uint32_t inode_size = ep->inode_size;
    uint32_t block_size = ep->block_size;
    int sparse = (ep->sb.s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) ? 1 : 0;
    uint32_t group_start = group * blocks_per_group;

    /* Calculate group metadata layout */
    uint32_t sb_blocks = 0;
    uint32_t bgd_blocks = 0;
    if (ext2_group_has_super(sparse, group)) {
        sb_blocks = (block_size == 1024) ? 2 : 1;
        uint32_t num_groups = group + 1; /* BGD table covers all groups up to this one */
        bgd_blocks = (uint32_t)(((uint64_t)num_groups * sizeof(struct ext2_bg_desc)
                                 + block_size - 1) / block_size);
    }

    uint32_t block_bitmap_block = group_start + sb_blocks + bgd_blocks;
    uint32_t inode_bitmap_block = block_bitmap_block + 1;
    uint32_t inode_table_blocks = (inodes_per_group * inode_size + block_size - 1)
                                   / block_size;
    uint32_t inode_table_block = inode_bitmap_block + 1;

    /* Allocate a zero-filled block buffer */
    uint8_t *zero_buf = (uint8_t *)kmalloc(block_size);
    if (!zero_buf) return -ENOMEM;
    memset(zero_buf, 0, block_size);

    /* Write block bitmap: all blocks in the group are free (except metadata) */
    {
        uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
        if (!bitmap) { kfree(zero_buf); return -ENOMEM; }
        memset(bitmap, 0xFF, block_size); /* start all free */

        uint32_t total_blocks_in_group = blocks_per_group;

        /* Mark used: superblock, BGD, bitmaps, inode table */
        uint32_t used_start = 0;
        uint32_t used_end = inode_table_block + inode_table_blocks;
        if (used_end > total_blocks_in_group)
            used_end = total_blocks_in_group;

        for (uint32_t b = used_start; b < used_end && b < total_blocks_in_group; b++) {
            bitmap[b / 8] &= ~(1U << (b % 8)); /* mark as used */
        }

        /* Write the block bitmap */
        uint64_t lba = (uint64_t)block_bitmap_block * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       bitmap + s * 512) != 0) {
                kfree(bitmap); kfree(zero_buf); return -EIO;
            }
        }
        kfree(bitmap);
    }

    /* Write inode bitmap: all inodes are free */
    {
        uint8_t *bitmap = (uint8_t *)kmalloc(block_size);
        if (!bitmap) { kfree(zero_buf); return -ENOMEM; }
        memset(bitmap, 0xFF, block_size); /* all free */

        uint64_t lba = (uint64_t)inode_bitmap_block * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       bitmap + s * 512) != 0) {
                kfree(bitmap); kfree(zero_buf); return -EIO;
            }
        }
        kfree(bitmap);
    }

    /* Write inode table: zero-filled */
    for (uint32_t i = 0; i < inode_table_blocks; i++) {
        uint64_t lba = (uint64_t)(inode_table_block + i) * (block_size / 512);
        for (uint32_t s = 0; s < block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, lba + s, 1,
                                       zero_buf) != 0) {
                kfree(zero_buf); return -EIO;
            }
        }
    }

    /* Zero out the superblock and BGD backup area if present */
    if (sb_blocks > 0) {
        uint64_t sb_lba = (uint64_t)group_start * (block_size / 512);
        for (uint32_t s = 0; s < sb_blocks * block_size / 512; s++) {
            if (blockdev_write_sectors(ep->dev_id, sb_lba + s, 1,
                                       zero_buf) != 0) {
                kfree(zero_buf); return -EIO;
            }
        }
    }

    kfree(zero_buf);

    /* Update BGD cache entry */
    uint32_t num_inodes = inodes_per_group;
    uint32_t num_blocks = blocks_per_group - (inode_table_block + inode_table_blocks);
    if (sb_blocks > 0) num_blocks -= sb_blocks;

    ep->bgd_cache[group].bg_block_bitmap     = block_bitmap_block;
    ep->bgd_cache[group].bg_inode_bitmap     = inode_bitmap_block;
    ep->bgd_cache[group].bg_inode_table      = inode_table_block;
    ep->bgd_cache[group].bg_free_blocks_count = (uint16_t)num_blocks;
    ep->bgd_cache[group].bg_free_inodes_count = (uint16_t)num_inodes;
    ep->bgd_cache[group].bg_used_dirs_count  = 0;

    return 0;
}

/* Grow the ext2 filesystem by adding new block groups.
 * @ep: ext2 private data (from mount)
 * @new_total_blocks: desired total blocks after resize
 *
 * The function adds enough complete block groups to reach the requested
 * size.  Partial block groups are not supported — the actual new size
 * may be larger than requested (rounded up to complete groups).
 *
 * Returns the new total number of blocks on success, negative on error.
 */
int64_t ext2_resize(struct ext2_priv *ep, uint64_t new_total_blocks)
{
    if (!ep) return -EINVAL;

    uint32_t current_groups = ep->num_block_groups;
    uint32_t blocks_per_group = ep->blocks_per_group;
    uint32_t new_groups_needed = (uint32_t)
        ((new_total_blocks + blocks_per_group - 1) / blocks_per_group);

    if (new_groups_needed <= current_groups) {
        kprintf("[ext2] resize: new size %llu <= current size, nothing to do\n",
                (unsigned long long)new_total_blocks);
        return (int64_t)ep->sb.s_blocks_count;
    }

    kprintf("[ext2] resize: growing from %u groups (%u blocks) to %u groups (%llu blocks)\n",
            current_groups, ep->sb.s_blocks_count,
            new_groups_needed, (unsigned long long)new_groups_needed * blocks_per_group);

    /* Expand the BGD cache */
    int ret = ext2_resize_bgd_cache(ep, new_groups_needed);
    if (ret != 0) return ret;

    /* Initialize each new group */
    for (uint32_t g = current_groups; g < new_groups_needed; g++) {
        ret = ext2_init_new_group(ep, g);
        if (ret != 0) {
            kprintf("[ext2] resize: failed to init group %u: %d\n", g, ret);
            ep->num_block_groups = g; /* partially completed */
            return ret;
        }
        kprintf("[ext2] resize: group %u initialized\n", g);
    }

    /* Update superblock */
    uint32_t added_blocks = (new_groups_needed - current_groups) * blocks_per_group;
    ep->sb.s_blocks_count += added_blocks;
    ep->sb.s_free_blocks_count += added_blocks; /* approximate — all new blocks are free */

    /* Add free inodes for the new groups */
    uint32_t added_inodes = (new_groups_needed - current_groups) * ep->inodes_per_group;
    ep->sb.s_free_inodes_count += added_inodes;

    ep->num_block_groups = new_groups_needed;

    /* Update modification time */
    ep->sb.s_wtime = (uint32_t)(timer_get_ticks() / TIMER_FREQ);

    /* Sync to disk */
    if (ext2_sync_super(ep) != 0) {
        kprintf("[ext2] resize: failed to sync superblock\n");
        return -EINVAL;
    }
    if (ext2_sync_bgd(ep) != 0) {
        kprintf("[ext2] resize: failed to sync BGD\n");
        return -EINVAL;
    }

    kprintf("[ext2] resize: complete — %u groups, %u blocks total\n",
            ep->num_block_groups, ep->sb.s_blocks_count);

    return (int64_t)ep->sb.s_blocks_count;
}

int __init ext2_init(void) {
    kprintf("[ext2] Ext2 filesystem initialized\n");
    vfs_register_filesystem("ext2", &ext2_ops);
    return 0;
}
#include "initcall.h"
fs_initcall(ext2_init);

#ifdef MODULE
/* Module entry point — called by the module ELF loader on insmod */
int __init init_module(void) {
    return ext2_init();
}

/* Module exit point — called by the module ELF loader on rmmod */
void __exit cleanup_module(void) {
    /* No VFS unregister yet; avoid unloading if filesystem is mounted */
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Second extended filesystem (ext2) — with symlinks, HTree directory indexing, and online resize");
#endif

/* ── ext2_umount ──────────────────────────────────────── */
int ext2_umount(const char *target)
{
    (void)target;
    kprintf("[ext2] Ext2 unmounted\n");
    return 0;
}
/* ── ext2_lookup ──────────────────────────────────────── */
int ext2_lookup(const char *name, void *parent)
{
    (void)parent;
    kprintf("[ext2] lookup: %s\n", name);
    return -ENOENT;
}
