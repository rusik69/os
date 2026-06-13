/* debugfs_enhance.c — debugfs enhancement: binary attributes, file ops groups
 *
 * Extends the basic debugfs with:
 *   - Binary attributes: raw read/write of binary data
 *   - File operations groups: batch register multiple files at once
 *   - Blob support: register pre-allocated data blobs
 *
 * These enhancements match sysfs_enhance.c but for the debugfs filesystem.
 */

#include "types.h"
#include "debugfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"
#include "vfs.h"
#include "errno.h"

/* ── Binary attribute support ──────────────────────────────────────── */

struct debugfs_bin_attr {
    char     name[DEBUGFS_MAX_NAME];
    void    *priv_data;
    size_t   size;
    int      (*read_fn)(struct debugfs_bin_attr *attr, uint64_t offset,
                        char *buf, size_t len);
    int      (*write_fn)(struct debugfs_bin_attr *attr, uint64_t offset,
                         const char *buf, size_t len);
    int      in_use;
};

#define DEBUGFS_MAX_BIN_ATTRS 64
static struct debugfs_bin_attr g_debugfs_bin_attrs[DEBUGFS_MAX_BIN_ATTRS];
static int g_debugfs_num_bin = 0;

/* ── Blob support ──────────────────────────────────────────────────── */

struct debugfs_blob {
    char        name[DEBUGFS_MAX_NAME];
    const void *data;
    size_t      size;
    int         in_use;
};

#define DEBUGFS_MAX_BLOBS 32
static struct debugfs_blob g_debugfs_blobs[DEBUGFS_MAX_BLOBS];
static int g_debugfs_num_blobs = 0;

/* ── Create a binary attribute ─────────────────────────────────────── */

int debugfs_create_bin_attr(const char *name, size_t size,
                             void *priv,
                             int (*read_fn)(struct debugfs_bin_attr *,
                                            uint64_t, char *, size_t),
                             int (*write_fn)(struct debugfs_bin_attr *,
                                             uint64_t, const char *, size_t))
{
    if (!name || g_debugfs_num_bin >= DEBUGFS_MAX_BIN_ATTRS)
        return -ENOSPC;

    int idx = g_debugfs_num_bin;
    struct debugfs_bin_attr *attr = &g_debugfs_bin_attrs[idx];

    strncpy(attr->name, name, DEBUGFS_MAX_NAME - 1);
    attr->size = size;
    attr->priv_data = priv;
    attr->read_fn = read_fn;
    attr->write_fn = write_fn;
    attr->in_use = 1;

    /* Create a regular debugfs entry as well */
    debugfs_create_file(name, NULL);

    g_debugfs_num_bin++;
    return idx;
}

/* ── Binary attribute read dispatch ────────────────────────────────── */

int debugfs_bin_read(const char *name, uint64_t offset,
                      char *buf, size_t len)
{
    for (int i = 0; i < g_debugfs_num_bin; i++) {
        if (g_debugfs_bin_attrs[i].in_use &&
            strcmp(g_debugfs_bin_attrs[i].name, name) == 0) {
            if (g_debugfs_bin_attrs[i].read_fn) {
                return g_debugfs_bin_attrs[i].read_fn(
                    &g_debugfs_bin_attrs[i], offset, buf, len);
            }
            return -ENOSYS;
        }
    }
    return -ENOENT;
}

/* ── Binary attribute write dispatch ───────────────────────────────── */

int debugfs_bin_write(const char *name, uint64_t offset,
                       const char *buf, size_t len)
{
    for (int i = 0; i < g_debugfs_num_bin; i++) {
        if (g_debugfs_bin_attrs[i].in_use &&
            strcmp(g_debugfs_bin_attrs[i].name, name) == 0) {
            if (g_debugfs_bin_attrs[i].write_fn) {
                return g_debugfs_bin_attrs[i].write_fn(
                    &g_debugfs_bin_attrs[i], offset, buf, len);
            }
            return -ENOSYS;
        }
    }
    return -ENOENT;
}

/* ── Blob registration ─────────────────────────────────────────────── */

int debugfs_create_blob(const char *name, const void *data, size_t size)
{
    if (!name || !data || g_debugfs_num_blobs >= DEBUGFS_MAX_BLOBS)
        return -ENOSPC;

    int idx = g_debugfs_num_blobs;
    struct debugfs_blob *blob = &g_debugfs_blobs[idx];

    strncpy(blob->name, name, DEBUGFS_MAX_NAME - 1);
    blob->data = data;
    blob->size = size;
    blob->in_use = 1;

    /* Create a read-only file for the blob */
    debugfs_create_file(name, NULL);

    g_debugfs_num_blobs++;
    return idx;
}

/* ── Read a blob (used by the VFS layer) ───────────────────────────── */

int debugfs_blob_read(const char *name, uint64_t offset,
                       char *buf, size_t len)
{
    for (int i = 0; i < g_debugfs_num_blobs; i++) {
        if (g_debugfs_blobs[i].in_use &&
            strcmp(g_debugfs_blobs[i].name, name) == 0) {
            size_t available = (offset < g_debugfs_blobs[i].size)
                ? (g_debugfs_blobs[i].size - (size_t)offset) : 0;
            size_t to_copy = (len < available) ? len : available;
            if (to_copy > 0) {
                memcpy(buf, (const char *)g_debugfs_blobs[i].data + offset,
                       to_copy);
            }
            return (int)to_copy;
        }
    }
    return -ENOENT;
}

/* ── File operations group support ─────────────────────────────────── */

struct debugfs_file_ops_group {
    const char                *name;
    const struct debugfs_ops *ops;
    int                        num_attrs;
    int                        in_use;
};

#define DEBUGFS_MAX_OPS_GROUPS 32
static struct debugfs_file_ops_group g_debugfs_groups[DEBUGFS_MAX_OPS_GROUPS];
static int g_debugfs_num_groups = 0;

int debugfs_register_ops_group(const char *name,
                                const struct debugfs_ops *ops,
                                int num_attrs)
{
    if (!name || !ops || g_debugfs_num_groups >= DEBUGFS_MAX_OPS_GROUPS)
        return -ENOSPC;

    int idx = g_debugfs_num_groups;
    struct debugfs_file_ops_group *grp = &g_debugfs_groups[idx];

    grp->name = name;
    grp->ops = ops;
    grp->num_attrs = num_attrs;
    grp->in_use = 1;

    for (int i = 0; i < num_attrs; i++) {
        debugfs_create_file(ops[i].name, ops[i].read);
    }

    g_debugfs_num_groups++;
    return idx;
}

/* ── Initialization ────────────────────────────────────────────────── */

void debugfs_enhance_init(void)
{
    memset(g_debugfs_bin_attrs, 0, sizeof(g_debugfs_bin_attrs));
    memset(g_debugfs_blobs, 0, sizeof(g_debugfs_blobs));
    memset(g_debugfs_groups, 0, sizeof(g_debugfs_groups));

    kprintf("[OK] DEBUGFS_ENHANCE: binary attributes, blobs, file ops groups active\n");
}
