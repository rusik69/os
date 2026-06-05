#ifndef VFS_H
#define VFS_H

#include "types.h"
#include "errno.h"

/* Maximum open files across all processes */
#define VFS_MAX_OPEN 32
/* Maximum filesystem mounts */
#define VFS_MAX_MOUNTS 16
/* Maximum filesystem type registrations */
#define VFS_MAX_FS_TYPES 16

struct fs_quota;
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
    uint64_t size;
    uint8_t  type;   /* 1=file, 2=dir */
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
    uint32_t mtime;
    uint32_t atime;
    uint32_t nlink;  /* link count */
    uint32_t ino;    /* inode number (0 = unknown/not applicable) */
};

/* POSIX advisory file lock */
struct file_lock {
    int      l_type;    /* F_RDLCK, F_WRLCK, F_UNLCK */
    int      l_whence;  /* SEEK_SET, SEEK_CUR, SEEK_END */
    int64_t  l_start;
    int64_t  l_len;     /* 0 = to EOF */
    int32_t  l_pid;
    int      used;
    int      mandatory; /* 1 = kernel-enforced mandatory lock */
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
    /* Optional: pre-allocate disk space */
    int (*fallocate)(void *priv, const char *path, int mode, uint32_t offset, uint32_t len);
    /* Optional: file deduplication (find and share identical blocks) */
    int (*dedup)(void *priv, const char *path1, const char *path2);
    /* Optional: filesystem resize (new_block_count) */
    int (*resize)(void *priv, uint32_t new_block_count);
    /* Optional: journal transaction operations */
    int (*journal_start)(void *priv);
    int (*journal_commit)(void *priv);
    int (*journal_abort)(void *priv);
    /* Symlink operations */
    int (*symlink)(void *priv, const char *target, const char *linkpath);
    int (*readlink)(void *priv, const char *path, char *buf, int bufsize);
    /* Optional: create device node */
    int (*mknod)(void *priv, const char *path, uint16_t mode,
                 uint16_t dev_major, uint16_t dev_minor);
    /* Optional: flush / sync all cached data for this filesystem to backing store */
    int (*flush)(void *priv);
    /* Optional: set file timestamps (atime, mtime).  times[0] = atime, times[1] = mtime.
     * Each entry uses tv_sec and tv_nsec; special values UTIME_NOW and UTIME_OMIT
     * are handled by the caller before dispatch.  Returns 0 on success or -errno. */
    int (*set_time)(void *priv, const char *path,
                    uint64_t atime_sec, uint64_t atime_nsec,
                    uint64_t mtime_sec, uint64_t mtime_nsec);
    /* Optional: rename/move a file or directory from old_path to new_path.
     * Both paths are on the same filesystem (the caller resolves mounts).
     * Returns 0 on success or negative errno on error.
     * If not provided, vfs_rename falls back to create+copy+delete. */
    int (*rename)(void *priv, const char *old_path, const char *new_path);
};

/* A mounted filesystem */
struct vfs_mount {
    char          mountpoint[64]; /* e.g. "/" */
    struct vfs_ops *ops;
    void          *priv;           /* private data passed to ops */
    int           flags;           /* mount flags (MS_RDONLY, etc.) */
    char          bind_source[64]; /* source path for bind mounts */
    int           is_bind;         /* 1 = bind mount */
    /* Journal state */
    int           journal_active;  /* 1 = in transaction */
    uint32_t      journal_seq;     /* transaction sequence number */
    /* Encryption state */
    int           encrypted;       /* 1 = encryption enabled */
    uint8_t       enc_key[16];    /* per-mount encryption key */
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
int vfs_append(const char *path, const void *data, uint32_t size);
int vfs_stat(const char *path, struct vfs_stat *st);
int vfs_create(const char *path, uint8_t type);
int vfs_unlink(const char *path);
int vfs_readdir(const char *path);

/* Rename/move a file or directory from old_path to new_path.
 * Both paths must be on the same filesystem (cross-fs rename returns -EXDEV).
 * Returns 0 on success or negative errno. */
int vfs_rename(const char *old_path, const char *new_path);

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

/* ── Set file timestamps (utimensat / futimens) ────────────────── */

/* Special tv_nsec values for utimensat/futimens */
#define UTIME_NOW  ((1UL << 30) - 1)   /* set to current time */
#define UTIME_OMIT ((1UL << 30) - 2)   /* leave unchanged */

/* Set atime and mtime for a path (nanosecond precision).
 * times[0] = atime, times[1] = mtime.  NULL times means set both to current time.
 * Returns 0 on success or negative errno. */
int vfs_set_time(const char *path, const struct timespec times[2]);

/* Set atime and mtime for an open file descriptor.
 * Same semantics as vfs_set_time but uses fd instead of path. */
int vfs_fset_time(int fd, const struct timespec times[2]);

/* Access time update */
void vfs_update_atime(const char *path);

/* Truncate file */
int vfs_truncate(const char *path, uint32_t len);

/* Hard link */
int vfs_link(const char *oldpath, const char *newpath);

/* Symbolic link operations */
int vfs_symlink(const char *target, const char *linkpath);
int vfs_readlink(const char *path, char *buf, int bufsize);

/* Create device node */
int vfs_mknod(const char *path, uint16_t mode, uint16_t dev_major, uint16_t dev_minor);

/* Bind mount support */
int vfs_bind_mount(const char *src, const char *target);
int vfs_is_bind_mount(const char *path);
const char *vfs_bind_source(const char *path);
/* Recursive bind mount: binds a subtree */
int vfs_bind_mount_recursive(const char *src, const char *target);
/* List all bind mounts */
int vfs_list_bind_mounts(char srcs[][64], char targets[][64], int max);

/* POSIX ACL */
int vfs_set_acl(const char *path, struct posix_acl *acl);
int vfs_get_acl(const char *path, struct posix_acl *acl);

/* Fallocate: pre-allocate disk space */
int vfs_fallocate(const char *path, int mode, uint32_t offset, uint32_t len);

/* Flush / sync operations */
int vfs_flush(const char *path);     /* flush a single filesystem by path */
int vfs_sync_all(void);              /* sync all mounted filesystems */

/*
 * Readahead at VFS level.
 *
 * Hints that the kernel should prefetch file data for the given byte range
 * into the page cache.  The actual behavior depends on the underlying
 * filesystem:
 *   - Block-device-backed filesystems (legacy SMFS): prefetch into page cache
 *   - Memory-backed filesystems (tmpfs, procfs, sysfs): no-op
 *
 * Returns 0 on success, or negative on error.
 */
int vfs_readahead(const char *path, uint32_t offset, uint32_t count);

/* File deduplication */
int vfs_dedup(const char *path1, const char *path2);

/* Filesystem resize */
int vfs_resize(const char *path, uint32_t new_block_count);

/* FS journal operations */
int vfs_journal_start(const char *path);
int vfs_journal_commit(const char *path);
int vfs_journal_abort(const char *path);

/* Pivot root — swap current root with new_root, stash old at put_old (Item 118) */
int vfs_pivot_root(const char *new_root, const char *put_old);

/*
 * vfs_force_readonly — remount a filesystem read-only on fatal error.
 *
 * Called by a filesystem driver when it detects unrecoverable corruption
 * (e.g. invalid superblock, corrupted inode bitmap, disk I/O failure on
 * a critical structure).  After this call, all write operations to the
 * affected filesystem will return -EROFS.
 *
 * @path    Any path within the filesystem to force read-only.
 * @reason  Human-readable description of the corruption (logged once).
 *
 * Returns 0 on success, -1 if the path has no matching mount.
 */
int vfs_force_readonly(const char *path, const char *reason);

/* Initramfs / CPIO extraction */
int cpio_extract_initramfs(uint32_t addr, uint32_t size);
int cpio_init(void);

/* tarfs init */
int tarfs_init(void);

/* ext2 init */
int ext2_init(void);

/* iso9660 init */
int iso9660_init(void);

/* romfs init */
int romfs_init(void);

/* FS encryption for SMTF */
int vfs_set_encryption(const char *path, int enabled);
int vfs_get_encryption(const char *path);

/* Block device cache stats extended */
void bufcache_stats_all(uint64_t *hits, uint64_t *misses, uint64_t *writes,
                        uint64_t *evictions, uint64_t *dirty_writes, uint32_t *ws_est);

/* FS quota enforcement at VFS level */
int vfs_set_quota(uint16_t uid, uint32_t block_limit, uint32_t inode_limit);
int vfs_get_quota(uint16_t uid, struct fs_quota *quota);
int vfs_check_quota_blocks(uint16_t uid, uint32_t blocks_needed);
int vfs_check_quota_inodes(uint16_t uid);

void vfs_init(void);

/* Mount table (extern for enhanced features) */
extern struct vfs_mount mounts[VFS_MAX_MOUNTS];
extern int num_mounts;

#endif
