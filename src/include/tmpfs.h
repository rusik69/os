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
 */

#define TMPFS_MAX_INODES 256
#define TMPFS_MAX_NAME   64
#define TMPFS_BLOCK_SIZE 4096

/* Inode types */
#define TMPFS_TYPE_FILE  1
#define TMPFS_TYPE_DIR   2
#define TMPFS_TYPE_LINK  3

/* In-memory inode */
struct tmpfs_inode {
    int      in_use;
    uint8_t  type;
    char     name[TMPFS_MAX_NAME];
    uint32_t size;
    uint8_t  *data;          /* file content (kmalloc'd) */
    uint8_t  uid, gid;
    uint16_t mode;
    uint32_t parent;         /* index of parent dir */
};

/* Mount an empty tmpfs — returns 0 on success */
int tmpfs_mount(void);

/* Unmount and free all data */
int tmpfs_unmount(void);

/* VFS operations for tmpfs */
extern struct vfs_ops tmpfs_vfs_ops;

/* Initialize tmpfs subsystem */
void tmpfs_init(void);

#endif
