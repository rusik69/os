#ifndef SQUASHFS_H
#define SQUASHFS_H

#include "types.h"

/*
 * On-disk SquashFS structures (simplified subset of the format).
 *
 * This implements a minimal read-only SquashFS that supports:
 *   - zlib compression
 *   - 4K block size
 *   - regular files and directories
 *   - UID/GID lookup
 *
 * Based on SquashFS 4.0 (little-endian on disk).
 */

/* Superblock magic */
#define SQUASHFS_MAGIC         0x73717368  /* "sqsh" */
#define SQUASHFS_MAGIC_LEN     4

/* Supported compression types */
#define SQUASHFS_COMPRESS_ZLIB 1

/* Block sizes (the kernel block_size in log2, e.g. 12 = 4096) */
#define SQUASHFS_LOG2_BLOCK_SIZE 12
#define SQUASHFS_BLOCK_SIZE    4096

/* Inode types */
#define SQUASHFS_DIR_TYPE      1
#define SQUASHFS_REG_TYPE      2
#define SQUASHFS_SYMLINK_TYPE  3

/* Superblock (on-disk, LE) */
struct squashfs_superblock {
    uint32_t magic;
    uint32_t inodes;
    uint32_t mtime;
    uint32_t block_size;
    uint32_t fragment_entry_count;
    uint16_t compression_id;
    uint16_t block_log;
    uint16_t flags;
    uint16_t no_ids;
    uint16_t s_major;
    uint16_t s_minor;
    uint64_t root_inode_ref;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t lookup_table_start;
} __attribute__((packed));

/* Inode header (base for all inode types) */
struct squashfs_inode_header {
    uint16_t type;
    uint16_t permissions;
    uint16_t uid_idx;
    uint16_t gid_idx;
    uint32_t mtime;
    uint32_t inode_number;
} __attribute__((packed));

/* Regular file inode (extends base header) */
struct squashfs_reg_inode {
    struct squashfs_inode_header header;
    uint32_t start_block;
    uint32_t fragment_block_index;
    uint32_t fragment_offset;
    uint32_t file_size;
    uint32_t block_sizes[];  /* array of compressed block sizes */
} __attribute__((packed));

/* Directory inode (extends base header) */
struct squashfs_dir_inode {
    struct squashfs_inode_header header;
    uint32_t block_index;
    uint32_t link_count;
    uint16_t file_size;
    uint16_t offset;
    uint32_t parent_inode;
} __attribute__((packed));

/* Directory entry */
struct squashfs_dir_entry {
    uint16_t offset;
    int16_t  inode_number_offset;
    uint16_t type;
    uint16_t name_size;
    char     name[];
} __attribute__((packed));

/* Directory header */
struct squashfs_dir_header {
    uint32_t count;          /* number of directory entries */
    uint32_t start_block;
    uint32_t inode_number;
} __attribute__((packed));

/* ── Public API ─────────────────────────────────────────────────────── */

/* Initialize the SquashFS filesystem support */
int squashfs_init(void);

/* Mount a SquashFS image at a given memory address */
int squashfs_mount(const char *mountpoint, uint32_t base_addr, uint32_t size);

#endif /* SQUASHFS_H */
