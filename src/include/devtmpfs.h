#ifndef DEV_TMPFS_H
#define DEV_TMPFS_H

#include "types.h"

/* Device types */
#define DT_CHAR  1
#define DT_BLOCK 2

/* Maximum device nodes in devtmpfs */
#define DEV_MAX_NODES 128

/* Device node descriptor */
struct devtmpfs_node {
    int      in_use;
    char     name[64];     /* device basename, e.g. "sda" */
    uint8_t  type;         /* DT_CHAR or DT_BLOCK */
    uint32_t major;
    uint32_t minor;
};

/* Create a device node at the given full path.
 * type is DT_CHAR or DT_BLOCK.
 * Returns 0 on success, negative on error. */
int devtmpfs_mknod(const char *path, uint8_t type, uint32_t major, uint32_t minor);

/* Convenience wrapper: creates a named device under /dev/<name>.
 * Equivalent to devtmpfs_mknod("/dev/<name>", ...). */
int devtmpfs_create_device(const char *name, uint8_t type, uint32_t major, uint32_t minor);

/* Populate /dev with standard device nodes (console, ttyS0, null, zero, etc.). */
int devtmpfs_setup(void);

/* Initialise the devtmpfs subsystem. */
void devtmpfs_init(void);

/* Access the global device table (for iteration / debugging).
 * Returns pointer to the static array and sets *count to the number of entries. */
struct devtmpfs_node *devtmpfs_get_table(int *count);

#endif /* DEV_TMPFS_H */
