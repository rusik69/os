#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAGIC       0x53464D53   /* "SMFS" */
#define FS_MAX_NAME    28
#define FS_MAX_FILES   128
#define FS_BLOCK_SIZE  512
#define FS_MAX_BLOCKS  8          /* max blocks per file = 4KB */
#define FS_DATA_START  34         /* sector: 0=super, 1-33=inodes (128 * 64B / 512 ~= 16 sectors, round up to 33) */

#define FS_TYPE_FREE   0
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2

/* 64 bytes per inode */
struct fs_inode {
    uint8_t  type;               /* FS_TYPE_* */
    uint8_t  reserved;
    uint16_t parent;             /* parent inode index */
    uint32_t size;               /* file size in bytes */
    uint32_t blocks[FS_MAX_BLOCKS]; /* sector numbers for data */
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
int fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size);
int fs_delete(const char *path);
int fs_list(const char *path);
int fs_stat(const char *path, uint32_t *size, uint8_t *type);
void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                  uint32_t *used_blocks, uint32_t *data_start);

#endif
