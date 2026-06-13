#ifndef EXT2_H
#define EXT2_H

#include "types.h"
#include "vfs.h"

/* Ext2 constants */
#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_ROOT_INO    2

/* Ext2 superblock */
struct ext2_superblock {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* EXT2 rev 1+ fields */
    uint32_t s_first_ino;           /* First non-reserved inode */
    uint16_t s_inode_size;          /* Size of each inode (128) */
    uint16_t s_block_group_nr;      /* Block group hosting this superblock */
    uint32_t s_feature_compat;      /* Compatible feature set */
    uint32_t s_feature_incompat;    /* Incompatible feature set */
    uint32_t s_feature_ro_compat;   /* Read-only compatible feature set */
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;         /* Compression algorithm bitmap */
    /* HTree-related fields */
    uint8_t  s_def_hash_version;    /* Default hash version for HTree */
    uint8_t  s_hash_seed[4];        /* Padding for hash seed alignment */
    uint32_t s_def_hash_seed[4];    /* Hash seed for HTree directory indexing */
} __attribute__((packed));

/* Ext2 block group descriptor */
struct ext2_bg_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

/* Ext2 inode */
struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15]; /* direct[12], indirect, double, triple */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed));

/* Feature flags — compatible */
#define EXT2_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT2_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT2_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT2_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT2_FEATURE_COMPAT_RESIZE_INO    0x0010
#define EXT2_FEATURE_COMPAT_DIR_INDEX     0x0020  /* HTree directory indexing */

/* Read-only compatible feature flags (s_feature_ro_compat) */
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE    0x0002
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR     0x0004

/* Incompatible feature flags (s_feature_incompat) */
#define EXT2_FEATURE_INCOMPAT_COMPRESSION   0x0001
#define EXT2_FEATURE_INCOMPAT_FILETYPE       0x0002
#define EXT2_FEATURE_INCOMPAT_RECOVER        0x0004
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV    0x0008
#define EXT2_FEATURE_INCOMPAT_META_BG        0x0010
#define EXT2_FEATURE_INCOMPAT_EXTENTS        0x0040
#define EXT2_FEATURE_INCOMPAT_64BIT          0x0080
#define EXT2_FEATURE_INCOMPAT_MMP            0x0100
#define EXT2_FEATURE_INCOMPAT_FLEX_BG        0x0200
#define EXT2_FEATURE_INCOMPAT_EA_INODE       0x0400
#define EXT2_FEATURE_INCOMPAT_DIRDATA        0x1000
#define EXT2_FEATURE_INCOMPAT_CSUM_SEED      0x2000
#define EXT2_FEATURE_INCOMPAT_LARGEDIR       0x4000
#define EXT2_FEATURE_INCOMPAT_INLINE_DATA    0x8000
#define EXT2_FEATURE_INCOMPAT_ENCRYPT        0x10000

/* Inode flags */
#define EXT2_INDEX_FL   0x00001000  /* Has HTree index */
#define EXT2_DIRSYNC_FL 0x00010000  /* Directory sync */

/* HTree hash versions */
#define EXT2_HTREE_LEGACY      0
#define EXT2_HTREE_HALF_MD4    1
#define EXT2_HTREE_TEA         2
#define EXT2_HTREE_LEGACY_UNSIGNED  3
#define EXT2_HTREE_HALF_MD4_UNSIGNED 4
#define EXT2_HTREE_TEA_UNSIGNED      5

/* HTree directory root structure (at beginning of first directory block) */
struct ext2_dx_root {
    uint32_t dot_inode;
    uint16_t dot_rec_len;
    uint8_t  dot_name_len;
    uint8_t  dot_type;
    char     dot_name[4];
    uint32_t dotdot_inode;
    uint16_t dotdot_rec_len;
    uint8_t  dotdot_name_len;
    uint8_t  dotdot_type;
    char     dotdot_name[4];
    uint32_t reserved;
    uint8_t  hash_version;
    uint8_t  info_length;       /* Should be 8 */
    uint8_t  indirect_levels;   /* Tree depth (0 = single level) */
    uint8_t  unused_flags;
    uint16_t limit;             /* Entry capacity */
    uint16_t count;             /* Entry count */
    uint32_t block;             /* Block number of this node */
    /* Followed by struct ext2_dx_entry entries[] */
} __attribute__((packed));

/* HTree index entry */
struct ext2_dx_entry {
    uint32_t hash;
    uint32_t block;
} __attribute__((packed));

/* HTree internal node (same structure but no dot/dotdot) */
struct ext2_dx_node {
    uint32_t reserved;
    uint8_t  hash_version;
    uint8_t  info_length;
    uint8_t  indirect_levels;
    uint8_t  unused_flags;
    uint16_t limit;
    uint16_t count;
    uint32_t block;
    /* Followed by struct ext2_dx_entry entries[] */
} __attribute__((packed));

/* ── Sparse superblock helpers ───────────────────────────────────────
 *
 * When EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER is set, superblock backups
 * exist only in block groups: 0, 1, and groups that are powers of 3, 5,
 * or 7.  All other groups have no superblock, which saves a lot of disk
 * space on large filesystems.
 *
 * For flex_bg (EXT2_FEATURE_INCOMPAT_FLEX_BG), the bitmap and inode
 * table metadata of several block groups are packed together into the
 * first group's space.  The bgd entries for subsequent groups in the
 * flex_bg point into the same physical blocks, making it transparent
 * to read operations that consult the bgd table.
 */

/* Test if a block group number has a superblock backup (sparse layout).
 * Returns 1 if yes, 0 if no.  For non-sparse filesystems, every group
 * has a superblock backup (always returns 1).
 * @sparse: 1 if EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER is set, 0 otherwise */
static inline int ext2_group_has_super(int sparse, uint32_t group)
{
    if (!sparse)
        return 1;  /* every group has a backup */
    if (group <= 1)
        return 1;
    /* Powers of 3, 5, and 7 — use uint64_t to avoid overflow */
    for (uint64_t p = 3; p <= group; p *= 3)  if ((uint32_t)p == group) return 1;
    for (uint64_t p = 5; p <= group; p *= 5)  if ((uint32_t)p == group) return 1;
    for (uint64_t p = 7; p <= group; p *= 7)  if ((uint32_t)p == group) return 1;
    return 0;
}

/* Compute the start block of a block group */
static inline uint32_t ext2_group_start(uint32_t group, uint32_t blocks_per_group)
{
    return group * blocks_per_group;
}

/* Compute the block number of the block group descriptor table within a
 * given block group.  This depends on whether the group has a superblock
 * backup (sparse layout) and the block size.
 *
 * For groups WITH a superblock:  bgd at block 1 (or 2 for 1024-byte blocks)
 * For groups WITHOUT a superblock: bgd at block 0 (right at group start)
 *
 * Parameters:
 *   group            — block group number
 *   blocks_per_group — blocks per group
 *   block_size       — filesystem block size in bytes
 *   sparse           — non-zero if SPARSE_SUPER feature is set */
static inline uint32_t ext2_bgd_block(uint32_t group, uint32_t blocks_per_group,
                                       uint32_t block_size, int sparse)
{
    uint32_t gstart = group * blocks_per_group;
    if (ext2_group_has_super(sparse, group))
        return gstart + ((block_size == 1024) ? 2 : 1);
    else
        return gstart;  /* No superblock, bgd starts at group start */
}

/* Return the size of the block group descriptor table in bytes */
static inline uint32_t ext2_bgd_table_size_blocks(uint32_t num_groups,
                                                    uint32_t block_size)
{
    uint32_t bgd_bytes = num_groups * sizeof(struct ext2_bg_desc);
    return (bgd_bytes + block_size - 1) / block_size;
}

/* ── ext2_priv forward declaration ────────────────────────────────────
 * The full struct definition is in ext2.c to keep the header clean.
 * Mount/unmount functions use this opaque pointer. */
struct ext2_priv;

/* Mount an ext2 filesystem from a block device */
int ext2_mount(const char *mountpoint, uint8_t dev_id);
int ext2_init(void);

/* Online resize: add block groups while mounted.
 * @ep: ext2 private data (from mount)
 * @new_total_blocks: desired total blocks after resize
 * Returns new total blocks on success, negative errno on failure. */
int64_t ext2_resize(struct ext2_priv *ep, uint64_t new_total_blocks);

#endif /* EXT2_H */
