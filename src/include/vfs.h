#ifndef VFS_H
#define VFS_H

#include "types.h"

/* Maximum open files across all processes */
#define VFS_MAX_OPEN 32
/* Maximum filesystem mounts */
#define VFS_MAX_MOUNTS 4
/* Maximum filesystem type registrations */
#define VFS_MAX_FS_TYPES 4

#define VFS_O_RDONLY 0
#define VFS_O_WRONLY 1
#define VFS_O_RDWR   2
#define VFS_O_CREAT  0x40

/* VFS stat result */
struct vfs_stat {
    uint32_t size;
    uint8_t  type;   /* 1=file, 2=dir */
};

/* Operations a filesystem must implement */
struct vfs_ops {
    /* Returns byte count read, or <0 on error */
    int (*read)(void *priv, const char *path, void *buf,
                uint32_t max_size, uint32_t *out_size);
    /* Returns 0 on success or <0 on error */
    int (*write)(void *priv, const char *path, const void *data, uint32_t size);
    /* Returns 0 on success or <0 on error */
    int (*stat)(void *priv, const char *path, struct vfs_stat *st);
    /* Returns 0 on success or <0 on error */
    int (*create)(void *priv, const char *path, uint8_t type);
    /* Returns 0 on success or <0 on error */
    int (*unlink)(void *priv, const char *path);
    /* List directory: calls kprintf per entry; returns 0 or <0 */
    int (*readdir)(void *priv, const char *path);
};

/* A mounted filesystem */
struct vfs_mount {
    char          mountpoint[64]; /* e.g. "/" */
    struct vfs_ops *ops;
    void          *priv;           /* private data passed to ops */
};

/* Register a filesystem type and mount it at a given path */
int vfs_mount(const char *mountpoint, struct vfs_ops *ops, void *priv);

/* VFS file operations — thin wrappers that resolve the mount */
int vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size);
int vfs_write(const char *path, const void *data, uint32_t size);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_create(const char *path, uint8_t type);
int vfs_unlink(const char *path);
int vfs_readdir(const char *path);

void vfs_init(void);

#endif
