#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "errno.h"

/* Maximum open files across all processes */
#define VFS_MAX_OPEN 32
/* Maximum filesystem mounts */
#define VFS_MAX_MOUNTS 8
/* Maximum filesystem type registrations */
#define VFS_MAX_FS_TYPES 4

#define VFS_O_RDONLY 0
#define VFS_O_WRONLY 1
#define VFS_O_RDWR   2
#define VFS_O_CREAT  0x40

/* Mount flags */
#define MS_RDONLY 1
#define MS_BIND   0x40

/* POSIX ACL tags */
#define ACL_USER_OBJ  1
#define ACL_USER      2
#define ACL_GROUP_OBJ 3
#define ACL_GROUP     4
#define ACL_MASK      5
#define ACL_OTHER     6

/* POSIX ACL entry */
struct posix_acl_entry {
    uint16_t tag;   /* ACL_USER_OBJ, ACL_USER, etc. */
    uint16_t perm;  /* permission bits (r/w/x) */
    uint32_t id;    /* user/group ID (for ACL_USER/ACL_GROUP) */
};

/* POSIX ACL: up to 3 entries */
#define POSIX_ACL_MAX_ENTRIES 3
struct posix_acl {
    struct posix_acl_entry entries[POSIX_ACL_MAX_ENTRIES];
    int count;
};

/* File lock types */
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2

/* File lock whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Xattr limits */
#define VFS_XATTR_PER_INODE 4
#define VFS_XATTR_NAME_MAX 16
#define VFS_XATTR_VALUE_MAX 64

/* VFS stat result */
struct vfs_stat {
    uint32_t size;
    uint8_t  type;   /* 1=file, 2=dir */
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
    uint32_t mtime;
    uint32_t atime;
    uint32_t nlink;  /* link count */
};

/* POSIX advisory file lock */
struct file_lock {
    int      l_type;    /* F_RDLCK, F_WRLCK, F_UNLCK */
    int      l_whence;  /* SEEK_SET, SEEK_CUR, SEEK_END */
    int64_t  l_start;
    int64_t  l_len;     /* 0 = to EOF */
    int32_t  l_pid;
    int      used;
    char     path_storage[64]; /* path this lock applies to */
};

/* Extended attribute entry */
struct xattr_entry {
    char  name[VFS_XATTR_NAME_MAX];
    char  value[VFS_XATTR_VALUE_MAX];
    int   size;
    int   in_use;
};

/* statfs structure */
struct vfs_statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_namelen;
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
    /* Optional: get directory entry names */
    int (*readdir_names)(void *priv, const char *path, char names[][64], int max);
    /* Optional: truncate file */
    int (*truncate)(void *priv, const char *path, uint32_t len);
};

/* A mounted filesystem */
struct vfs_mount {
    char          mountpoint[64]; /* e.g. "/" */
    struct vfs_ops *ops;
    void          *priv;           /* private data passed to ops */
    int           flags;           /* mount flags (MS_RDONLY, etc.) */
    char          bind_source[64]; /* source path for bind mounts */
    int           is_bind;         /* 1 = bind mount */
};

/* Registered filesystem type */
struct vfs_filesystem_type {
    char name[32];
    struct vfs_ops *ops;
    int registered;
};

/* Register a filesystem type and mount it at a given path */
int vfs_mount_ex(const char *mountpoint, struct vfs_ops *ops, void *priv, int flags);
int vfs_mount(const char *mountpoint, struct vfs_ops *ops, void *priv);

/* Register a filesystem type (for /proc/filesystems) */
int vfs_register_filesystem(const char *name, struct vfs_ops *ops);

/* List registered filesystem types */
int vfs_list_filesystems(char names[][32], int max);

/* VFS file operations — thin wrappers that resolve the mount */
int vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size);
int vfs_write(const char *path, const void *data, uint32_t size);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_create(const char *path, uint8_t type);
int vfs_unlink(const char *path);
int vfs_readdir(const char *path);

/* Fill names[] with up to max directory entries; returns count or <0 */
int vfs_readdir_names(const char *path, char names[][64], int max);

/* List mounted filesystem paths; returns count */
int vfs_list_mountpoints(char mounts[][64], int max);

/* File locking */
int vfs_setlk(const char *path, struct file_lock *flk, int wait);
int vfs_getlk(const char *path, struct file_lock *flk);

/* Extended attributes */
int vfs_setxattr(const char *path, const char *name, const void *value, int size);
int vfs_getxattr(const char *path, const char *name, void *value, int size);
int vfs_listxattr(const char *path, char *buf, int size);

/* Filesystem statistics */
int vfs_statfs(const char *path, struct vfs_statfs *st);
int vfs_fstatfs(int fd, struct vfs_statfs *st);

/* Access time update */
void vfs_update_atime(const char *path);

/* Truncate file */
int vfs_truncate(const char *path, uint32_t len);

/* Hard link */
int vfs_link(const char *oldpath, const char *newpath);

/* Bind mount support */
int vfs_bind_mount(const char *src, const char *target);
int vfs_is_bind_mount(const char *path);
const char *vfs_bind_source(const char *path);

/* POSIX ACL */
int vfs_set_acl(const char *path, struct posix_acl *acl);
int vfs_get_acl(const char *path, struct posix_acl *acl);

void vfs_init(void);

#endif
