#ifndef REISERFS_H
#define REISERFS_H

#include "types.h"
#include "vfs.h"

/* ReiserFS constants */
#define REISERFS_MAGIC_STR    "ReIsErFs"
#define REISERFS2_MAGIC_STR   "ReIsEr2Fs"
#define REISERFS3_MAGIC_STR   "ReIsEr3Fs"
#define REISERFS_MAGIC_LEN    9  /* includes terminator? no — 8 chars + maybe \0? strings are 8 chars */
#define REISERFS_SUPER_OFFSET 0x10000  /* 64KB offset */
#define REISERFS_ROOT_INO     1        /* root inode number in reiserfs is 1 */

/* ReiserFS superblock (at offset 0x10000) */
struct reiserfs_superblock {
    uint32_t s_block_count;         /* blocks count */
    uint32_t s_free_blocks;         /* free blocks */
    uint32_t s_root_block;          /* root block number */
    uint32_t s_journal_block;       /* journal block number */
    uint32_t s_journal_dev;         /* journal device number */
    uint32_t s_orig_journal_size;   /* original journal size */
    uint32_t s_journal_trans_max;   /* max transactions */
    uint32_t s_journal_magic;       /* journal magic */
    uint32_t s_journal_max_batch;   /* max batch */
    uint32_t s_journal_max_commit_age; /* max commit age */
    uint32_t s_journal_max_commit_age_s; /* max commit age in seconds */
    uint32_t s_blocksize;           /* block size */
    uint32_t s_oid_maxsize;         /* max object id size */
    uint32_t s_oid_cursize;         /* current object id size */
    uint32_t s_state;               /* state flags */
    char     s_magic[16];           /* magic string */
    uint16_t s_tree_height;         /* tree height */
    uint16_t s_bmap_nr;             /* bitmap blocks count */
    uint16_t s_version;             /* version */
    uint16_t s_reserved;
    uint32_t s_inode_generation;
    uint32_t s_flags;
} __attribute__((packed));

/* ReiserFS block header (every block starts with this) */
struct reiserfs_block_header {
    uint16_t blk_level;         /* level in B* tree: LEAF=1, INTERNAL=2 */
    uint16_t blk_nr_items;      /* number of items in this block */
    uint16_t blk_free_space;    /* free space in block */
    uint16_t blk_reserved;
} __attribute__((packed));

/* ReiserFS item header (in a leaf block) */
struct reiserfs_item_header {
    uint32_t ih_key;            /* the key as a directory cookie */
    uint32_t ih_key_off;        /* objectid + offset in directory */
    uint32_t ih_count;          /* count of entries in direct item */
    uint16_t ih_item_len;       /* length of item data */
    uint16_t ih_item_loc;       /* location of item data (from block start) */
    uint16_t ih_free_space;     /* free space in item */
    uint16_t ih_reserved;
} __attribute__((packed));

/* ReiserFS key format: packing locality + objectid + offset + type */
struct reiserfs_key {
    uint32_t k_dir_id;          /* packing locality (directory ID) */
    uint32_t k_objectid;        /* object ID (inode number) */
    uint32_t k_offset;          /* byte offset in file, or dir entry hash */
    uint32_t k_type;            /* item type */
} __attribute__((packed));

/* Stat item data (describes a file/directory) */
struct reiserfs_stat_item {
    uint16_t sd_mode;
    uint16_t sd_reserved;
    uint32_t sd_size;
    uint32_t sd_uid;
    uint32_t sd_gid;
    uint32_t sd_atime;
    uint32_t sd_mtime;
    uint32_t sd_ctime;
    uint32_t sd_blocks;
    uint32_t sd_rdev;
} __attribute__((packed));

/* Directory entry */
struct reiserfs_dir_entry {
    uint32_t de_dir_id;         /* directory ID */
    uint32_t de_objectid;       /* object ID (inode number) */
    char     de_name[0];        /* variable-length name */
} __attribute__((packed));

/* Item types */
#define REISERFS_STAT_ITEM    0
#define REISERFS_DIR_ITEM     1  /* directory entries */
#define REISERFS_DIRECT_ITEM  2  /* file data (small files) */
#define REISERFS_INDIRECT_ITEM 3 /* file data (indirect blocks) */

/* B* tree levels */
#define REISERFS_BLK_LEAF     1
#define REISERFS_BLK_INTERNAL 2

/* Key type bits for directory items: offset has dir-entry lookup info */
#define REISERFS_DIR_ENTRY_KEY_TYPE_SHIFT 60
#define REISERFS_MAX_NAME_LEN 255

/* Forward declaration */
struct reiserfs_priv;

/* Public API */
int reiserfs_init(void);

#endif /* REISERFS_H */
