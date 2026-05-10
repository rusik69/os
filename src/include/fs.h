#ifndef FS_H
#define FS_H

#include "types.h"

#define FS_MAGIC       0x53464D54   /* "SMFT" v2 – with permissions */
#define FS_MAX_NAME    28
#define FS_MAX_FILES   128
#define FS_BLOCK_SIZE  512
#define FS_MAX_BLOCKS  16         /* max blocks per file = 8KB */
#define FS_DATA_START  40         /* sector: 0=super, 1-39=inodes */

#define FS_TYPE_FREE   0
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2

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

/* Inode — on-disk structure */
struct fs_inode {
    uint8_t  type;               /* FS_TYPE_* */
    uint8_t  _pad;
    uint16_t parent;             /* parent inode index */
    uint32_t size;               /* file size in bytes */
    uint32_t blocks[FS_MAX_BLOCKS]; /* sector numbers for data */
    uint16_t uid;                /* owning user id  */
    uint16_t gid;                /* owning group id */
    uint16_t mode;               /* permission bits (octal, includes sticky) */
    uint8_t  _pad2[6];           /* reserved / future use */
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
int fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
               uint16_t *uid, uint16_t *gid, uint16_t *mode);
int fs_chmod(const char *path, uint16_t mode);
int fs_chown(const char *path, uint16_t uid, uint16_t gid);
int fs_check_perm(const char *path, char op); /* op: 'r','w','x' */
void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                  uint32_t *used_blocks, uint32_t *data_start);
int fs_list_names(const char *dir, const char *prefix,
                  char names[][FS_MAX_NAME], int max);

/* Format a mode word as "rwxrwxrwx" into a 9-char buffer (+ NUL) */
void fs_mode_str(uint16_t mode, char out[10]);

#endif
