#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAGIC       0x53464D57   /* "SMFT" v5 – 256 inodes, 256-block files, mtime */
#define FS_MAX_NAME    28
#define FS_MAX_FILES   256
#define FS_BLOCK_SIZE  512
#define FS_MAX_BLOCKS  256        /* max blocks per file = 128KB */
/* Data starts after superblock (1) + inode table (computed at format time) */

#define FS_TYPE_FREE   0
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2
#define FS_TYPE_LINK   3  /* symbolic link — target stored in first data block */

/* Unix-style permission bits (stored in inode.mode) */
#define FS_PERM_RUSR   0400   /* owner read    */
#define FS_PERM_WUSR   0200   /* owner write   */
#define FS_PERM_XUSR   0100   /* owner execute */
#define FS_PERM_RGRP   0040   /* group read    */
#define FS_PERM_WGRP   0020   /* group write   */
#define FS_PERM_XGRP   0010   /* group execute */
#define FS_PERM_ROTH   0004   /* other read    */
#define FS_PERM_WOTH   0002   /* other write   */
#define FS_PERM_XOTH   0001   /* other execute */
#define FS_PERM_STICKY 01000  /* sticky bit (restricted deletion on dirs) */

#define FS_MODE_FILE   0644   /* default file: rw-r--r-- */
#define FS_MODE_DIR    0755   /* default dir:  rwxr-xr-x */

/* Inode — on-disk structure (grows with FS_MAX_BLOCKS) */
struct fs_inode {
    uint8_t  type;               /* FS_TYPE_* */
    uint8_t  _pad;
    uint16_t parent;             /* parent inode index */
    uint32_t size;               /* file size in bytes */
    uint32_t blocks[FS_MAX_BLOCKS]; /* sector numbers for data */
    uint16_t uid;                /* owning user id  */
    uint16_t gid;                /* owning group id */
    uint16_t mode;               /* permission bits (octal, includes sticky) */
    uint32_t mtime;              /* modification time (seconds since boot) */
    uint8_t  _pad2[2];           /* reserved */
    char     name[FS_MAX_NAME];
} __attribute__((packed));

/* Superblock at sector 0 */
struct fs_super {
    uint32_t magic;
    uint32_t num_inodes;
    uint32_t num_data_blocks;
    uint32_t next_free_block;     /* next sector to allocate */
    uint8_t  padding[496];
} __attribute__((packed));

void fs_init(void);
int fs_format(void);
int fs_create(const char *path, uint8_t type);
int fs_write_file(const char *path, const void *data, uint32_t size);
int fs_append(const char *path, const void *data, uint32_t len);
int fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size);
int fs_delete(const char *path);
int fs_list(const char *path);
int fs_stat(const char *path, uint32_t *size, uint8_t *type);
int fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
               uint16_t *uid, uint16_t *gid, uint16_t *mode);
int fs_stat_mtime(const char *path);  /* returns mtime seconds, or -1 */
int fs_set_mtime(const char *path, uint32_t mtime);  /* set mtime seconds, or -1 on error */
void fs_register_page_cache_writeback(void);
int fs_chmod(const char *path, uint16_t mode);
int fs_chown(const char *path, uint16_t uid, uint16_t gid);
int fs_check_perm(const char *path, char op); /* op: 'r','w','x' */
void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                  uint32_t *used_blocks, uint32_t *data_start);
int fs_list_names(const char *dir, const char *prefix,
                  char names[][FS_MAX_NAME], int max);
int fs_symlink(const char *path, const char *target);
int fs_readlink(const char *path, char *buf, int bufsize);
int fs_lstat(const char *path, uint32_t *size, uint8_t *type); /* stat without following symlinks */
int fs_truncate(const char *path, uint32_t len); /* truncate file to len bytes */

/* Format a mode word as "rwxrwxrwx" into a 9-char buffer (+ NUL) */
void fs_mode_str(uint16_t mode, char out[10]);

/* ── Quota support ────────────────────────────────────────────── */
struct fs_quota {
    uint16_t uid;
    uint32_t block_soft_limit;   /* soft limit in 512-byte blocks (0 = unlimited) */
    uint32_t block_hard_limit;   /* hard limit in 512-byte blocks (0 = unlimited) */
    uint32_t inode_soft_limit;   /* soft limit in inodes (0 = unlimited) */
    uint32_t inode_hard_limit;   /* hard limit in inodes (0 = unlimited) */
    uint32_t cur_blocks;         /* current block usage */
    uint32_t cur_inodes;         /* current inode usage */
    int      block_grace;        /* 1 = block grace period active */
    int      inode_grace;        /* 1 = inode grace period active */
};

/* Set quota limits for a user ID */
int fs_set_quota(uint16_t uid, uint32_t block_limit, uint32_t inode_limit);

/* Get quota for a user ID */
int fs_get_quota(uint16_t uid, struct fs_quota *quota);

/* Check quota before allocating blocks/inodes (returns 0 if allowed, -1 if denied) */
int fs_check_quota_blocks(uint16_t uid, uint32_t blocks_needed);
int fs_check_quota_inodes(uint16_t uid);

/* ── Copy-on-Write for data blocks ────────────────────────────── */
/* When a block is referenced by multiple inodes, allocate a new block,
 * copy the data, and update the inode. Returns the new block number or 0. */
uint32_t fs_cow_block(uint32_t block);

#endif
