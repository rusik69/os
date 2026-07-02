#ifndef EXT2_H
#define EXT2_H

#include "types.h"
#include "vfs.h"

/* Ext2 constants */
#define EXT2_SUPER_MAGIC 0xEF53
#define EXT2_ROOT_INO    2

/* Superblock revision levels */
#define EXT2_GOOD_OLD_REV       0  /* Revision 0 — original ext2, 128-byte inodes */
#define EXT2_DYNAMIC_REV        1  /* Revision 1 — variable inode size, features */

/* Default values for GOOD_OLD_REV */
#define EXT2_GOOD_OLD_INODE_SIZE    128
#define EXT2_GOOD_OLD_FIRST_INO     11

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
    uint32_t s_last_orphan;         /* Inode number of head of orphan list (0 = none) */
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

#define EXT2_INLINE_DATA_FL 0x10000000 /* Inode has inline data */

/* Directory entry file type values (when FILETYPE incompat feature set) */
#define EXT2_FT_UNKNOWN    0
#define EXT2_FT_REG_FILE   1
#define EXT2_FT_DIR        2
#define EXT2_FT_CHRDEV     3
#define EXT2_FT_BLKDEV     4
#define EXT2_FT_FIFO       5
#define EXT2_FT_SOCK       6
#define EXT2_FT_SYMLINK    7

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
    size_t bgd_bytes = num_groups * sizeof(struct ext2_bg_desc);
    return (uint32_t)((bgd_bytes + block_size - 1) / block_size);
}

/* ── Extended attribute (EA) on-disk structures ───────────────────────
 *
 * Extended attributes are stored in a separate block pointed to by the
 * i_file_acl field of the inode.  The EA block has a header followed by
 * packed entries, with entry names stored inline and values packed
 * backwards from the end of the block.
 *
 * Entry names are stored WITHOUT the namespace prefix — the e_name_index
 * field encodes which namespace the entry belongs to (user, system,
 * security, or trusted).
 *
 * The on-disk format is compatible with Linux's ext2/3 EA layout.
 * ═══════════════════════════════════════════════════════════════════════ */
#define EXT2_EXT_ATTR_MAGIC        0xEA020000  /* EA block magic number */

#define EXT2_XATTR_INDEX_USER        0   /* user. namespace */
#define EXT2_XATTR_INDEX_SYSTEM      1   /* system. namespace */
#define EXT2_XATTR_INDEX_SECURITY    2   /* security. namespace */
#define EXT2_XATTR_INDEX_TRUSTED     3   /* trusted. namespace */

/* Round up to 4-byte alignment */
#define EXT2_EXT_ATTR_ROUND         3U
#define EXT2_EXT_ATTR_ALIGN(len)    (((len) + EXT2_EXT_ATTR_ROUND) & ~EXT2_EXT_ATTR_ROUND)

/* Size of an entry including the name (rounded up to 4 bytes) */
#define EXT2_EXT_ATTR_ENTRY_LEN(namelen) \
    EXT2_EXT_ATTR_ALIGN(sizeof(struct ext2_ext_attr_entry) + (namelen))

/* Advance to the next entry in the EA block */
#define EXT2_EXT_ATTR_NEXT(entry) \
    ((struct ext2_ext_attr_entry *)((uint8_t *)(entry) + \
      EXT2_EXT_ATTR_ENTRY_LEN((entry)->e_name_len)))

/* EA block header */
struct ext2_ext_attr_header {
    uint32_t h_magic;        /* 0xEA020000 */
    uint32_t h_refcount;     /* Number of inodes sharing this block */
    uint32_t h_blocks;       /* Number of blocks used (must be 1) */
    uint32_t h_hash;         /* Hash of all attributes */
    uint32_t h_checksum;     /* Checksum (ext4, unused for ext2) */
    uint32_t h_reserved[3];  /* Reserved */
} __attribute__((packed));

/* EA entry (followed by name bytes, no null terminator) */
struct ext2_ext_attr_entry {
    uint8_t  e_name_len;      /* Length of name (without namespace prefix) */
    uint8_t  e_name_index;    /* Namespace index (EXT2_XATTR_INDEX_*) */
    uint16_t e_value_offs;    /* Byte offset of value from block start */
    uint32_t e_value_block;   /* Disk block for value (0 = same block) */
    uint32_t e_value_size;    /* Size of value in bytes */
    uint32_t e_hash;          /* Hash of entry name */
} __attribute__((packed));

/* ── Extended attribute function declarations ─────────────────────── */

/* Forward declaration of ext2_priv (defined later in this header) */
struct ext2_priv;

/* Get an extended attribute value from an ext2 inode.
 * @ep: ext2 private data
 * @inode: ext2 inode (on-disk format)
 * @name: full xattr name including namespace prefix
 * @buf: output buffer for the value
 * @size: size of output buffer
 * Returns: number of bytes read on success, negative errno on failure */
int ext2_ea_get(struct ext2_priv *ep, uint32_t ino,
                struct ext2_inode *inode,
                const char *name, void *buf, size_t size);

/* Set an extended attribute on an ext2 inode.
 * If name already exists, its value is replaced.
 * If the inode has no EA block, one is allocated.
 * @ep: ext2 private data
 * @inode: ext2 inode (modified in-place; caller must write back via
 *         ext2_write_inode if the function returns 0)
 * @name: full xattr name including namespace prefix
 * @value: value to store
 * @size: size of value in bytes
 * Returns: 0 on success, negative errno on failure */
int ext2_ea_set(struct ext2_priv *ep, uint32_t ino,
                struct ext2_inode *inode,
                const char *name, const void *value, size_t size);

/* List extended attribute names on an ext2 inode.
 * Names are written as null-terminated strings into @buf.
 * @ep: ext2 private data
 * @inode: ext2 inode
 * @buf: output buffer
 * @size: size of output buffer
 * Returns: total bytes written (sum of null-terminated name lengths)
 *         on success, negative errno on failure.
 *         If @buf is NULL, returns the total bytes needed. */
int ext2_ea_list(struct ext2_priv *ep, struct ext2_inode *inode,
                 char *buf, size_t size);

/* Remove an extended attribute from an ext2 inode.
 * If this was the last EA, the EA block is freed and i_file_acl is
 * cleared in @inode.  Caller must write the inode back afterwards.
 * @ep: ext2 private data
 * @inode: ext2 inode (modified in-place)
 * @name: full xattr name including namespace prefix
 * Returns: 0 on success, -ENODATA if not found, negative errno on error */
int ext2_ea_remove(struct ext2_priv *ep, uint32_t ino,
                   struct ext2_inode *inode,
                   const char *name);

/* ── ext2 private data (per-mount) ─────────────────────────────────────
 * The full struct is defined here so that filesystem utility modules
 * (e.g. ext2_ea.c) can access the members directly. */
struct ext2_priv {
    uint8_t  dev_id;
    uint32_t block_size;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t num_block_groups;
    struct ext2_superblock sb;
    char     mountpoint[64];

    /* Cached block group descriptor table */
    struct ext2_bg_desc *bgd_cache;
    uint32_t             bgd_cache_size;
};

/* ── Internal block I/O (used by ext2_ea.c and ext2.c) ────────────── */

/* Mark a filesystem as corrupted (remounts read-only) */
int ext2_corrupt(struct ext2_priv *ep, const char *reason);

/* Read one block from the block device */
int ext2_read_block(struct ext2_priv *ep, uint32_t block_num, uint8_t *buf);

/* Write one block to the block device */
int ext2_write_block(struct ext2_priv *ep, uint32_t block_num,
                     const uint8_t *buf);

/* Read an inode from disk */
int ext2_read_inode(struct ext2_priv *ep, uint32_t ino,
                    struct ext2_inode *inode);

/* Write an inode back to disk */
int ext2_write_inode(struct ext2_priv *ep, uint32_t ino,
                     const struct ext2_inode *inode);

/* Allocate one free block */
int ext2_alloc_block(struct ext2_priv *ep, uint32_t *block_out);

/* Free a previously allocated block (marks bitmap, updates superblock) */
int ext2_free_block(struct ext2_priv *ep, uint32_t block_num);

/* Mount an ext2 filesystem from a block device */
int ext2_mount(const char *mountpoint, uint8_t dev_id);
int ext2_init(void);

/* Restore the primary superblock from a backup copy.
 * Scans all block groups that have superblock backups, finds the first
 * valid one, and writes it to the primary location.
 * Returns 0 on success, -EFSCORRUPTED if no valid backup found. */
int ext2_restore_super_from_backup(struct ext2_priv *ep);

/* ── POSIX ACL operations (ext2_acl.c) ──────────────────────────────── */

/* Set a POSIX ACL on an ext2 inode via the extended attribute block.
 * @ep: ext2 private data
 * @ino: inode number
 * @inode: ext2 inode (updated if EA block allocated)
 * @name: "system.posix_acl_access" or "system.posix_acl_default"
 * @acl: ACL to store
 * Returns 0 on success, negative errno on failure. */
int ext2_acl_set(struct ext2_priv *ep, uint32_t ino,
                  struct ext2_inode *inode,
                  const char *name, const struct posix_acl *acl);

/* Get a POSIX ACL from an ext2 inode via the extended attribute block.
 * @ep: ext2 private data
 * @ino: inode number
 * @inode: ext2 inode
 * @name: "system.posix_acl_access" or "system.posix_acl_default"
 * @acl: output buffer for the ACL
 * Returns 0 on success, -ENODATA if no ACL, negative errno on error. */
int ext2_acl_get(struct ext2_priv *ep, uint32_t ino,
                  struct ext2_inode *inode,
                  const char *name, struct posix_acl *acl);

/* Remove a POSIX ACL from an ext2 inode.
 * @ep: ext2 private data
 * @ino: inode number
 * @inode: ext2 inode (updated if EA block freed)
 * @name: "system.posix_acl_access" or "system.posix_acl_default"
 * Returns 0 on success, -ENODATA if not found, negative errno on error. */
int ext2_acl_remove(struct ext2_priv *ep, uint32_t ino,
                     struct ext2_inode *inode,
                     const char *name);

/* Check permission on an ext2 inode with ACL support.
 * Consults the POSIX ACL from the EA block first, then falls back
 * to traditional mode bits.
 * @ep: ext2 private data
 * @path: path (for VFS-level fallback, may be NULL)
 * @ino: inode number
 * @inode: ext2 inode
 * @uid: requesting user
 * @gid: requesting group
 * @mode: file mode bits (i_mode)
 * @file_uid: file owner
 * @file_gid: file group
 * @op: required permission (4=read, 2=write, 1=execute)
 * Returns 0 (allowed) or -EACCES. */
int ext2_acl_permission(struct ext2_priv *ep, const char *path,
                         uint32_t ino, struct ext2_inode *inode,
                         uint16_t uid, uint16_t gid,
                         uint16_t mode, uint16_t file_uid,
                         uint16_t file_gid, uint16_t op);

/* Online resize: add block groups while mounted.
 * @ep: ext2 private data (from mount)
 * @new_total_blocks: desired total blocks after resize
 * Returns new total blocks on success, negative errno on failure. */
int64_t ext2_resize(struct ext2_priv *ep, uint64_t new_total_blocks);

/* ── Orphan handling ──────────────────────────────────────────────────── */

/* Add an inode to the orphan list.
 * The orphan list is a singly-linked list rooted at s_last_orphan in the
 * superblock; each orphan inode stores the next orphan's inode number in
 * its i_dtime field (0 = end of list).  Called when i_links_count drops
 * to 0 but the inode may still be referenced by open file descriptors.
 * @ep: ext2 private data
 * @ino: inode number of the orphaned inode
 * @inode: the on-disk inode (i_dtime is updated to store timestamp)
 * Returns 0 on success, negative errno on failure. */
int ext2_orphan_add(struct ext2_priv *ep, uint32_t ino,
                    struct ext2_inode *inode);

/* Remove an inode from the orphan list.
 * Walks the orphan linked list and unlinks @ino.  The inode's i_dtime
 * is preserved (not cleared) — caller should zero it if desired.
 * @ep: ext2 private data
 * @ino: inode number to remove from orphan list
 * Returns 0 on success, -ENOENT if inode was not on the list. */
int ext2_orphan_del(struct ext2_priv *ep, uint32_t ino);

/* Fully release an orphaned inode: remove from orphan list, free all
 * data blocks, and free the inode itself.  The inode bitmap and block
 * bitmaps are updated, and the superblock is synced.
 * @ep: ext2 private data
 * @ino: inode number to release
 * Returns 0 on success, negative errno on failure. */
int ext2_orphan_release(struct ext2_priv *ep, uint32_t ino);

/* Process the entire orphan list: for each orphaned inode, free its
 * data blocks, free the inode, and remove it from the list.  After
 * cleanup, s_last_orphan is set to 0.
 * @ep: ext2 private data
 * Returns 0 on success, negative errno on failure. */
int ext2_orphan_cleanup(struct ext2_priv *ep);

#endif /* EXT2_H */
