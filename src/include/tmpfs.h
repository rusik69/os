#ifndef TMPFS_H
#define TMPFS_H

#include "types.h"
#include "vfs.h"

/*
 * tmpfs — RAM-backed filesystem for /tmp, /dev/shm, etc.
 *
 * All data lives in kernel heap memory. Supports files, directories,
 * and symbolic links. Simple flat inode table (no tree).
 *
 * Mount:
 *   vfs_mount("/tmp", &tmpfs_vfs_ops, NULL);
 *
 * Internally uses the VFS stat structure for metadata and kmalloc'd
 * buffers for file contents.
 *
 * Size limits: per-mount size quotas can be set via tmpfs_mount_with_limit().
 * When the limit is exceeded, write operations return -ENOSPC.
 */

#define TMPFS_MAX_INODES 256
#define TMPFS_MAX_NAME   64
#define TMPFS_BLOCK_SIZE 4096

/* Default size limit when none is specified (0 = unlimited). */
#define TMPFS_SIZE_UNLIMITED 0ULL

/* Inode types */
#define TMPFS_TYPE_FILE  1
#define TMPFS_TYPE_DIR   2
#define TMPFS_TYPE_LINK  3
#define TMPFS_TYPE_CHR   4  /* character device node */
#define TMPFS_TYPE_BLK   5  /* block device node */

/* Device number for mknod */
struct tmpfs_devno {
    uint16_t major;
    uint16_t minor;
};

/* In-memory inode */
struct tmpfs_inode {
    int      in_use;
    uint8_t  type;
    char     name[TMPFS_MAX_NAME];
    uint32_t size;
    uint8_t  *data;          /* file content (kmalloc'd), or symlink target */
    uint8_t  uid, gid;
    uint16_t mode;
    uint32_t parent;         /* index of parent dir */
    struct tmpfs_devno dev;  /* device major/minor for device nodes */
};

/* Mount an empty tmpfs — returns 0 on success */
int tmpfs_mount(void);

/* Mount tmpfs with a per-mount size limit (in bytes).
 * Writes that would exceed this limit return -ENOSPC.
 * Pass TMPFS_SIZE_UNLIMITED (0) for no limit. */
int tmpfs_mount_with_limit(uint64_t max_bytes);

/* Unmount and free all data */
int tmpfs_unmount(void);

/* VFS operations for tmpfs */
extern struct vfs_ops tmpfs_vfs_ops;

/* Initialize tmpfs subsystem */
void tmpfs_init(void);

#endif
