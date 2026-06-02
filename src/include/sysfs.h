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

/** Write callback for writable sysfs files. Returns 0 on success, -1 on error. */
typedef int (*sysfs_write_cb_t)(const char *data, uint32_t size);

/** Read callback for dynamic sysfs files. Returns the number of bytes written to buf. */
typedef int (*sysfs_read_cb_t)(char *buf, uint32_t max_size);

struct sysfs_entry {
    char     name[SYSFS_MAX_NAME];
    uint8_t  type;        /* 1=file, 2=dir */
    char     content[256]; /* static content for files (used when read_cb == NULL) */
    uint32_t size;
    int      parent;      /* index of parent dir (-1 = root) */
    int      in_use;
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
 * write_cb is called on write with the data and size. Returns 0 on success. */
int sysfs_create_writable_file(const char *path, const char *initial_content,
                                sysfs_read_cb_t read_cb, sysfs_write_cb_t write_cb);

/* Create a directory under /sys/<path> */
int sysfs_create_dir(const char *path);

#endif /* SYSFS_H */
