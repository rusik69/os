#ifndef SYSFS_H
#define SYSFS_H

#include "types.h"
#include "vfs.h"

/* sysfs — read-only virtual filesystem exposing kernel objects.
 * Mounted at /sys. Pre-populated with /sys/class/, /sys/block/,
 * /sys/devices/, /sys/kernel/.
 *
 * Files have static content; directories contain children.
 * Maximum: 64 entries.
 */

#define SYSFS_MAX_ENTRIES 64
#define SYSFS_MAX_NAME    48

struct sysfs_entry {
    char     name[SYSFS_MAX_NAME];
    uint8_t  type;        /* 1=file, 2=dir */
    char     content[256]; /* static content for files */
    uint32_t size;
    int      parent;      /* index of parent dir (-1 = root) */
    int      in_use;
};

/* VFS operations */
extern struct vfs_ops sysfs_vfs_ops;

/* Initialise sysfs and pre-populate directories */
void sysfs_init(void);

/* Create a virtual file under /sys/<path> with static content */
int sysfs_create_file(const char *path, const char *content);

/* Create a directory under /sys/<path> */
int sysfs_create_dir(const char *path);

#endif /* SYSFS_H */
