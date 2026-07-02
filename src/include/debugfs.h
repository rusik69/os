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
 *   - Hierarchical directories (type 2)
 *
 * Maximum: 128 entries (hierarchical directories + files).
 */
#define DEBUGFS_MAX_ENTRIES 128
#define DEBUGFS_MAX_NAME    48

/* Entry type constants */
#define DEBUGFS_TYPE_FILE   1   /* regular file (read, write, or rw) */
#define DEBUGFS_TYPE_DIR    2   /* directory */

/* Read callback: fill buf with content, set *len to byte count */
typedef void (*debugfs_read_fn)(char *buf, int *len);

/* Write callback: receive data of given size */
typedef int (*debugfs_write_fn)(const char *buf, int len);

/* Release callback: called when entry is removed (may be NULL).
 * @priv is the private data pointer passed at entry creation.
 * The callback should free any dynamically allocated resources. */
typedef void (*debugfs_release_cb)(void *priv);

struct debugfs_entry {
    char     name[DEBUGFS_MAX_NAME];
    uint8_t  type;           /* DEBUGFS_TYPE_FILE or DEBUGFS_TYPE_DIR */
    int      parent;         /* index of parent (-1 for root) */
    int      in_use;
    /* File-specific fields (valid when type == DEBUGFS_TYPE_FILE) */
    debugfs_read_fn  read_fn;
    debugfs_write_fn write_fn;
    uint32_t *u32_val;       /* for u32-type files (type 2 in old API) */
    void    *priv;           /* private data passed to read/write callbacks */
    debugfs_release_cb release_cb; /* called on remove (may be NULL) */
};

/* Initialise debugfs — mounts on /sys/kernel/debug */
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

/* Create a file under a specific parent directory.
 * @name: entry basename (no '/' allowed)
 * @read_fn: read callback (may be NULL)
 * @write_fn: write callback (may be NULL)
 * @parent: parent debugfs directory entry pointer (NULL = root)
 * Returns 0 on success, -1 on error. */
int debugfs_create_file_in_dir(const char *name,
                               void (*read_fn)(char *buf, int *len),
                               int (*write_fn)(const char *buf, int len),
                               void *parent);

/* Create a directory in debugfs.
 * @name: directory name (relative to parent)
 * @parent: parent entry (NULL = root /sys/kernel/debug)
 * Returns opaque pointer to the directory entry on success,
 * NULL on failure. */
void *debugfs_create_dir(const char *name, void *parent);

/* Remove a single debugfs entry (file or empty directory).
 * Returns 0 on success, -1 if not found or not empty. */
int debugfs_remove(void *entry);

/* Remove a directory and all its children recursively. */
int debugfs_remove_recursive(void *entry);

/* Set the release callback for an existing debugfs entry.
 * The callback is invoked when the entry is removed, allowing
 * the owner to free dynamically allocated resources. */
int debugfs_set_release_cb(void *entry, debugfs_release_cb cb);

/* VFS operations */
extern struct vfs_ops debugfs_vfs_ops;

#endif /* DEBUGFS_H */
