#ifndef SYSFS_H
#define SYSFS_H

#include "types.h"
#include "vfs.h"

/* sysfs — virtual filesystem exposing kernel objects.
 * Mounted at /sys. Pre-populated with /sys/class/, /sys/block/,
 * /sys/devices/, /sys/kernel/.
 *
 * Files can have static content or dynamic read/write callbacks.
 * Maximum: 64 entries.
 */

#define SYSFS_MAX_ENTRIES 64
#define SYSFS_MAX_NAME    48

/** Write callback for writable sysfs files. Returns 0 on success, -1 on error.
 *  @priv is the private data pointer passed at file creation. */
typedef int (*sysfs_write_cb_t)(const char *data, uint32_t size, void *priv);

/** Read callback for dynamic sysfs files. Returns the number of bytes written to buf.
 *  @priv is the private data pointer passed at file creation. */
typedef int (*sysfs_read_cb_t)(char *buf, uint32_t max_size, void *priv);

struct sysfs_entry {
    char     name[SYSFS_MAX_NAME];
    uint8_t  type;        /* 1=file, 2=dir */
    char     content[256]; /* static content for files (used when read_cb == NULL) */
    uint32_t size;
    int      parent;      /* index of parent dir (-1 = root) */
    int      in_use;
    void    *priv;        /* private data passed to read/write callbacks */
    sysfs_read_cb_t  read_cb;   /* dynamic read callback (overrides static content) */
    sysfs_write_cb_t write_cb;  /* write callback for writable files (NULL = read-only) */
};

/* VFS operations */
extern struct vfs_ops sysfs_vfs_ops;

/* Initialise sysfs and pre-populate directories */
void sysfs_init(void);

/* Create a virtual file under /sys/<path> with static content */
int sysfs_create_file(const char *path, const char *content);

/* Create a writable virtual file with dynamic read/write callbacks.
 * If read_cb is NULL, static content is used. If write_cb is NULL, the file is read-only.
 * write_cb is called on write with the data, size, and priv. Returns 0 on success. */
int sysfs_create_writable_file(const char *path, const char *initial_content,
                                void *priv,
                                sysfs_read_cb_t read_cb, sysfs_write_cb_t write_cb);

/* Create a directory under /sys/<path> */
int sysfs_create_dir(const char *path);

/* Remove a single sysfs entry (file or empty directory).
 * Returns 0 on success, -1 if the entry doesn't exist or cannot be removed. */
int sysfs_remove(const char *path);

/* Remove a directory and all its children recursively.
 * Returns 0 on success, -1 if not found. */
int sysfs_remove_recursive(const char *path);

#endif /* SYSFS_H */
