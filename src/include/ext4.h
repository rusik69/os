#ifndef EXT4_H
#define EXT4_H

#include "types.h"
#include "vfs.h"
#include "jbd2.h"

/* Ext4 is backward-compatible with ext2/3 — same magic */
#define EXT4_SUPER_MAGIC   0xEF53
#define EXT4_ROOT_INO      2
#define EXT4_MAX_BLOCK_SIZE 4096

/* Filesystem state flags (s_state) */
#define EXT4_VALID_FS    0x0001  /* Unmounted cleanly */
#define EXT4_ERROR_FS    0x0002  /* Errors detected */
#define EXT4_ORPHAN_FS   0x0004  /* Orphans being recovered */

/* Behaviour when errors detected (s_errors) */
#define EXT4_ERRORS_CONTINUE 1  /* Continue execution */
#define EXT4_ERRORS_RO       2  /* Remount read-only */
#define EXT4_ERRORS_PANIC    3  /* Panic */

/* Revision levels */
#define EXT4_GOOD_OLD_REV 0
#define EXT4_DYNAMIC_REV  1

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
#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER    0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE      0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR       0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE       0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM        0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK       0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE     0x0040
#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM   0x0400

/* Checksum type constants for s_checksum_type */
#define EXT4_CRC32_CHKSUM   0
#define EXT4_CRC32C_CHKSUM  1

/* Inline data: data stored in i_block[0..14] (60 bytes max for ext4) */
#define EXT4_INLINE_DATA_FL 0x10000000  /* Inode flag for inline data */
#define EXT4_MAX_INLINE_DATA 60         /* bytes in i_block[0..14] for inline data */

/* Extent flag on inode */
#define EXT4_EXTENTS_FL 0x00080000

/* Default mount options (s_default_mount_opts) */
#define EXT4_DEFM_DEBUG        0x0001  /* Print debug messages */
#define EXT4_DEFM_BSDGROUPS    0x0002  /* BSD group semantics */
#define EXT4_DEFM_XATTR_USER   0x0004  /* User extended attributes */
#define EXT4_DEFM_ACL          0x0008  /* POSIX ACL support */
#define EXT4_DEFM_UID16        0x0010  /* 16-bit UIDs */
#define EXT4_DEFM_JMODE_DATA   0x0020  /* Journal data mode */
#define EXT4_DEFM_JMODE_ORDER  0x0040  /* Journal ordered mode (default) */
#define EXT4_DEFM_JMODE_WBACK  0x0080  /* Journal writeback mode */
#define EXT4_DEFM_NOBARRIER    0x0100  /* Disable barriers */
#define EXT4_DEFM_BLOCK_VALIDITY 0x0200 /* Block validity checks */
#define EXT4_DEFM_DISCARD      0x0400  /* Issue discards on free */
#define EXT4_DEFM_NODELALLOC   0x0800  /* Disable delayed allocation */

/* Journal states for ext4_priv */
#define EXT4_JOURNAL_NONE    0  /* No journal present */
#define EXT4_JOURNAL_PRESENT 1  /* Journal present, no recovery needed */
#define EXT4_JOURNAL_RECOVER 2  /* Journal present, recovery needed */
#define EXT4_JOURNAL_DEVICE  3  /* Journal on external device */

/* Ext4 superblock (full 1024-byte on-disk structure, packed).
 * First ~200 bytes are ext2-compatible, then ext4-specific fields follow. */
struct ext4_superblock {
    /* ── EXT2/3 compatible fields (0–83) ── */
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

    /* ── EXT2_DYNAMIC_REV fields (84–199) ── */
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

    /* ── Performance hints (200–223) ── */
    uint8_t  s_prealloc_blocks;       /* 200 */
    uint8_t  s_prealloc_dir_blocks;   /* 201 */
    uint16_t s_reserved_gdt_blocks;   /* 202–203 */

    /* ── Journaling info (204–231) ── */
    uint8_t  s_journal_uuid[16];      /* 204–219: UUID of journal superblock */
    uint32_t s_journal_inum;          /* 220–223: Journal inode number */
    uint32_t s_journal_dev;           /* 224–227: Journal device number */
    uint32_t s_last_orphan;           /* 228–231: Orphan inode list head */

    /* ── Directory indexing (232–255) ── */
    uint32_t s_hash_seed[4];          /* 232–247: HTree hash seed */
    uint8_t  s_def_hash_version;      /* 248 */
    uint8_t  s_jnl_backup_type;       /* 249 */
    uint16_t s_desc_size;             /* 250–251: Group descriptor size */

    /* ── Mount and block group info (252–267) ── */
    uint32_t s_default_mount_opts;    /* 252–255: Default mount options */
    uint32_t s_first_meta_bg;         /* 256–259: First meta block group */
    uint32_t s_mkfs_time;             /* 260–263: Creation timestamp */
    uint32_t s_jnl_blocks[17];        /* 264–331: Journal inode backup */

    /* ── 64-bit block counts (332–347) ── */
    uint32_t s_blocks_count_hi;       /* 332–335: Blocks count high 32 bits */
    uint32_t s_r_blocks_count_hi;     /* 336–339: Reserved blocks high 32 bits */
    uint32_t s_free_blocks_hi;        /* 340–343: Free blocks high 32 bits */
    uint16_t s_min_extra_isize;       /* 344–345: Minimum inode extra size */
    uint16_t s_want_extra_isize;      /* 346–347: Desired inode extra size */

    /* ── Misc flags and RAID (348–371) ── */
    uint32_t s_flags;                 /* 348–351: Miscellaneous flags */
    uint16_t s_raid_stride;           /* 352–353: RAID stride */
    uint16_t s_mmp_interval;          /* 354–355: MMP check interval (sec) */
    uint64_t s_mmp_block;             /* 356–363: MMP block address */
    uint32_t s_raid_stripe_width;     /* 364–367: RAID stripe width */
    uint8_t  s_log_groups_per_flex;   /* 368: Flex groups per flex_bg (log2) */
    uint8_t  s_checksum_type;         /* 369: Metadata checksum type */
    uint16_t s_reserved_pad;          /* 370–371 */

    /* ── Lifetime & snapshot (372–399) ── */
    uint64_t s_kbytes_written;        /* 372–379: Lifetime KiB written */
    uint32_t s_snapshot_inum;         /* 380–383: Snapshot inode */
    uint32_t s_snapshot_id;           /* 384–387: Snapshot ID */
    uint64_t s_snapshot_r_blocks_count; /* 388–395: Snapshot reserved blocks */
    uint32_t s_snapshot_list;         /* 396–399: Snapshot list head */

    /* ── Error tracking (400–511) ── */
    uint32_t s_error_count;           /* 400–403 */
    uint32_t s_first_error_time;      /* 404–407 */
    uint32_t s_first_error_ino;       /* 408–411 */
    uint64_t s_first_error_block;     /* 412–419 */
    uint8_t  s_first_error_func[32];  /* 420–451 */
    uint32_t s_first_error_line;      /* 452–455 */
    uint32_t s_last_error_time;       /* 456–459 */
    uint32_t s_last_error_ino;        /* 460–463 */
    uint32_t s_last_error_line;       /* 464–467 */
    uint64_t s_last_error_block;      /* 468–475 */
    uint8_t  s_last_error_func[32];   /* 476–507 */
    uint8_t  s_mount_opts[64];        /* 508–571 */

    /* ── Quota & overlay (572–631) ── */
    uint32_t s_usr_quota_inum;        /* 572–575 */
    uint32_t s_grp_quota_inum;        /* 576–579 */
    uint32_t s_overhead_blocks;       /* 580–583 */
    uint32_t s_backup_bgs[2];         /* 584–591 */
    uint8_t  s_encrypt_algos[4];      /* 592–595 */
    uint8_t  s_encrypt_pw_salt[16];   /* 596–611 */
    uint32_t s_lpf_ino;               /* 612–615: Lost+found inode */
    uint32_t s_prj_quota_inum;        /* 616–619: Project quota inode */
    uint32_t s_checksum_seed;         /* 620–623: Metadata checksum seed */

    /* ── Padding and superblock checksum (624–1023) ── */
    uint8_t  s_reserved[392];         /* 624–1015 */
    uint32_t s_checksum;              /* 1016–1019: Superblock CRC32 */
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

/* ── Quota inode numbers ──────────────────────────────────────────── */
#define EXT4_USR_QUOTA_INO  3   /* Inode number for user quota file */
#define EXT4_GRP_QUOTA_INO  4   /* Inode number for group quota file */
#define EXT4_PRJ_QUOTA_INO  5   /* Inode number for project quota file */

/* Quota format versions */
#define EXT4_QFMT_VFS_OLD   1   /* VFS quota format v0 (old) */
#define EXT4_QFMT_VFS_V0    2   /* VFS quota format v0 */
#define EXT4_QFMT_VFS_V1    4   /* VFS quota format v1 (64-bit, project quotas) */

/* On-disk quota block entry (vfsv0/v1 format).
 *
 * Each disk quota block contains an array of these entries, packed
 * sequentially.  The blocks are stored in the quota inode (pointed to
 * by s_usr_quota_inum / s_grp_quota_inum / s_prj_quota_inum in the
 * superblock).  A zero dqb_valid means the entry is free/unused.
 *
 * Field layout matches the Linux VFS v1 disk quota format. */
struct ext4_dqblk {
    uint32_t dqb_id;              /* UID, GID, or project ID */
    uint64_t dqb_bhardlimit;      /* absolute limit on disk blocks (bytes) */
    uint64_t dqb_bsoftlimit;      /* preferred limit on disk blocks (bytes) */
    uint64_t dqb_curspace;        /* current space occupied (bytes) */
    uint64_t dqb_ihardlimit;      /* maximum # allocated inodes */
    uint64_t dqb_isoftlimit;      /* preferred inode limit */
    uint64_t dqb_curinodes;       /* current # allocated inodes */
    uint64_t dqb_btime;           /* time limit for exceeding block soft limit */
    uint64_t dqb_itime;           /* time limit for exceeding inode soft limit */
    uint32_t dqb_valid;           /* 1 if entry is valid/used, 0 if free */
    uint32_t pad3[3];             /* padding to 104 bytes total */
} __attribute__((packed));

/* Quota info header (vfsv0/v1 format).
 * Stored at offset 0 of the quota inode, after a tree header block. */
struct ext4_dqinfo {
    uint32_t dqi_bgrace;          /* block grace time (seconds) */
    uint32_t dqi_igrace;          /* inode grace time (seconds) */
    uint32_t dqi_flags;           /* quota flags */
    uint32_t dqi_blocks;          /* total blocks in quota file */
    uint32_t dqi_free_blk;        /* first free block index */
    uint32_t dqi_free_entry;      /* first free entry index */
    uint32_t pad;
} __attribute__((packed));

/* ── Project quota helpers ────────────────────────────────────────── */

/* Extract the project ID from an ext4 inode.
 * Returns 0 if the inode extra space is too small to hold i_projid. */
static inline uint32_t
ext4_get_projid(const struct ext4_inode *inode, uint16_t inode_size)
{
    /* i_projid is at offset 128 + i_extra_isize - 4 in the ext4 inode.
     * If extra_isize is large enough to include it, return it. */
    uint16_t extra_isize = inode->i_extra_isize;
    if (extra_isize >= sizeof(uint32_t) &&
        (uint32_t)(128 + extra_isize) <= (uint32_t)inode_size) {
        return inode->i_projid;
    }
    return 0; /* No project ID assigned */
}

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
    char     name[];         /* variable-length name (up to 255 bytes) */
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

/* ── Private per-mount data (defined here for companion modules) ── */
struct ext4_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t num_block_groups;
    struct ext4_superblock sb;
    char     mountpoint[64];

    /* Cached block group descriptor table */
    struct ext4_bg_desc *bgd_cache;
    uint32_t             bgd_cache_size;

    /* Feature flags (cached for fast access) */
    uint32_t incompat;
    uint32_t ro_compat;
    uint32_t compat;

    /* flex_bg: number of block groups in a flex_bg group */
    uint32_t flex_bg_size; /* 0 if flex_bg not enabled */

    /* JBD2 journal for metadata journaling (NULL if no journal) */
    struct jbd2_journal *journal;
    /* Current JBD2 transaction handle (NULL if no active transaction) */
    struct jbd2_handle *journal_handle;

    /* ── Quota support fields ── */
    uint32_t usr_quota_inum;    /* inode of user quota file (0 if none) */
    uint32_t grp_quota_inum;    /* inode of group quota file (0 if none) */
    uint32_t prj_quota_inum;    /* inode of project quota file (0 if none) */
    uint32_t quota_format;      /* quota format version (0 = none) */
};

/* Corruption helper — forces read-only remount */
int ext4_corrupt(struct ext4_priv *ep, const char *reason);

/* Block I/O — read one block from the backing device */
int ext4_read_block(struct ext4_priv *ep, uint32_t block_num, uint8_t *buf);

/*
 * ext4_inode_get_blocks — return the full 512-byte block count for an inode.
 *
 * Without HUGE_FILE, i_blocks is a plain 32-bit counter (max ~2TB of 512-byte
 * sectors).  With HUGE_FILE, the upper 16 bits are stored in i_osd2[0..1]
 * (Linux's l_i_blocks_high), forming a 48-bit counter that supports files
 * far beyond 2TB.
 *
 * @inode:     on-disk inode
 * @huge_file: non-zero if EXT4_FEATURE_RO_COMPAT_HUGE_FILE is set
 *
 * Returns the full block count in 512-byte units.
 */
static inline uint64_t
ext4_inode_get_blocks(const struct ext4_inode *inode, int huge_file)
{
    uint64_t blocks = inode->i_blocks;

    if (huge_file) {
        uint16_t blocks_hi;
        memcpy(&blocks_hi, &inode->i_osd2[0], sizeof(blocks_hi));
        blocks |= ((uint64_t)blocks_hi << 32);
    }

    return blocks;
}

/* Public API */
int ext4_mount(const char *mountpoint, uint8_t dev_id);
int ext4_init(void);

/* Checksum verification for ext4 metadata structures (CRC32C) */
int ext4_verify_sb_checksum(struct ext4_priv *ep);
int ext4_verify_bg_checksum(struct ext4_priv *ep,
                             const struct ext4_bg_desc *bg,
                             uint32_t bg_index);
int ext4_verify_inode_checksum(struct ext4_priv *ep,
                                const struct ext4_inode *inode,
                                uint32_t ino);

/* Project quota support */
int  ext4_read_quota_block(struct ext4_priv *ep, uint32_t projid,
                            struct ext4_dqblk *dqblk);
int  ext4_init_quota(struct ext4_priv *ep);
void ext4_log_quota_info(struct ext4_priv *ep);
#endif /* EXT4_H */
