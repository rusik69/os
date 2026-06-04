#ifndef DEBUGFS_H
#define DEBUGFS_H

#include "types.h"
#include "vfs.h"

/* debugfs — debug virtual filesystem mounted at /sys/kernel/debug/.
 *
 * Supports:
 *   - Files with dynamic read callbacks
 *   - u32 variables exposed as read/write files
 *   - Files with both read and write callbacks (type 3)
 *
 * Maximum: 32 entries.
 */

#define DEBUGFS_MAX_ENTRIES 32
#define DEBUGFS_MAX_NAME    48

/* Read callback: fill buf with content, set *len to byte count */
typedef void (*debugfs_read_fn)(char *buf, int *len);

/* Write callback: receive data of given size */
typedef int (*debugfs_write_fn)(const char *buf, int len);

struct debugfs_entry {
    char     name[DEBUGFS_MAX_NAME];
    uint8_t  type;           /* 1=callback read, 2=u32, 3=callback rw */
    debugfs_read_fn  read_fn;
    debugfs_write_fn write_fn;
    uint32_t *u32_val;       /* for type 2 (u32 files) */
    int      in_use;
};

/* Initialise debugfs */
void debugfs_init(void);

/* Create a file with dynamic read callback */
int debugfs_create_file(const char *name,
                        void (*read_fn)(char *buf, int *len));

/* Create a read-write file with both read and write callbacks.
 * Returns 0 on success, -1 if the entry table is full or the name
 * already exists. */
int debugfs_create_rw_file(const char *name,
                           void (*read_fn)(char *buf, int *len),
                           int (*write_fn)(const char *buf, int len));

/* Create a u32 variable file (read/write) */
int debugfs_create_u32(const char *name, uint32_t *val);

/* VFS operations */
extern struct vfs_ops debugfs_vfs_ops;

#endif /* DEBUGFS_H */
