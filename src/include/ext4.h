#ifndef EXT4_H
#define EXT4_H

#include "types.h"
#include "vfs.h"

/* Ext4 is backward-compatible with ext2/3 — same magic */
#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_ROOT_INO      2

/* Ext4 superblock (same as ext2 — first 120 bytes, then ext4 extensions) */
struct ext4_superblock {
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
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* Performance hints */
    uint8_t  s_def_hash_version;
    uint8_t  s_hash_seed[4];
    uint32_t s_def_hash_seed[4];
} __attribute__((packed));

/* Ext4 block group descriptor (same as ext2, but with checksum in bg_reserved) */
struct ext4_bg_desc {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
} __attribute__((packed));

/* Ext4 inode (same as ext2, then 32+ bytes ext4 extras) */
struct ext4_inode {
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
    uint32_t i_block[15]; /* direct[12], indirect, double, triple, OR extent root */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_size_high; /* i_dir_acl in ext2 — upper 32 bits of size */
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
    /* Ext4 extended fields (beyond 128 bytes) */
    uint16_t i_extra_isize;
    uint16_t i_checksum_hi;
    uint32_t i_ctime_extra;
    uint32_t i_mtime_extra;
    uint32_t i_atime_extra;
    uint32_t i_crtime;
    uint32_t i_crtime_extra;
    uint32_t i_version_hi;
    uint32_t i_projid;
} __attribute__((packed));

/* Ext4 extent tree structures */

/* Extent header — sits at i_block[0] when EXT4_EXTENTS_FL is set */
#define EXT4_EXTENTS_FL 0x00080000

struct ext4_extent_header {
    uint16_t eh_magic;      /* 0xF30A */
    uint16_t eh_entries;    /* number of valid entries */
    uint16_t eh_max;        /* capacity of entries in this block */
    uint16_t eh_depth;      /* 0 = leaf, >0 = index */
    uint32_t eh_generation; /* generation number */
} __attribute__((packed));

/* Extent index node (internal tree node) */
struct ext4_extent_idx {
    uint32_t ei_block;      /* first logical block covered */
    uint32_t ei_leaf_lo;    /* lower 32 bits of physical block */
    uint16_t ei_leaf_hi;    /* upper 16 bits of physical block */
    uint16_t ei_unused;
} __attribute__((packed));

/* Extent leaf node */
struct ext4_extent {
    uint32_t ee_block;      /* first logical block covered */
    uint16_t ee_len;        /* number of blocks covered (len <= 32768) */
    uint16_t ee_start_hi;   /* upper 16 bits of physical block */
    uint32_t ee_start_lo;   /* lower 32 bits of physical block */
} __attribute__((packed));

/* Ext4 directory entry (with file_type) */
struct ext4_dir_entry {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[0];       /* variable-length name (up to 255 bytes) */
} __attribute__((packed));

/* File types for ext4 directory entries */
#define EXT4_FT_UNKNOWN   0
#define EXT4_FT_REG_FILE  1
#define EXT4_FT_DIR       2
#define EXT4_FT_CHRDEV    3
#define EXT4_FT_BLKDEV    4
#define EXT4_FT_FIFO      5
#define EXT4_FT_SOCK      6
#define EXT4_FT_SYMLINK   7
#define EXT4_FT_MAX       8

/* Incompat feature flags */
#define EXT4_FEATURE_INCOMPAT_COMPRESSION   0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE      0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER       0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV   0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG       0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS       0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT         0x0080
#define EXT4_FEATURE_INCOMPAT_MMP           0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG       0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE      0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA       0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED     0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR      0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA   0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT       0x10000

/* Compat feature flags */
#define EXT4_FEATURE_COMPAT_DIR_PREALLOC  0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES 0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL   0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR      0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INO    0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX     0x0020

/* RO compat feature flags */
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER  0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE    0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR     0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE     0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM      0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK     0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE   0x0040

/* Inline data: data stored in i_block[0..14] (60 bytes max for ext4) */
#define EXT4_INLINE_DATA_FL 0x10000000  /* Inode flag for inline data */
#define EXT4_MAX_INLINE_DATA 60         /* bytes in i_block[0..14] for inline data */

/* Forward declaration */
struct ext4_priv;

/* Public API */
int ext4_mount(const char *mountpoint, uint8_t dev_id);
int ext4_init(void);

#endif /* EXT4_H */
